#include "http_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "sdkconfig.h"

#include "driver/gpio.h"

#include "board_pins.h"
#include "usb_backend.h"
#include "virtual_device.h"

#define HTTP_PORT              80
#define HTTP_MAX_CLIENTS       4
#define HTTP_BUF_SIZE          2048
#define HTTP_TASK_STACK        16384
#define HTTP_TASK_PRIORITY     3

static const char *TAG = "http_server";

/* ─────────────────────────────────────────────
 *  Streaming chunked writer — writes directly
 *  to the socket with HTTP chunked encoding.
 *  No fixed-size output buffer needed.
 * ───────────────────────────────────────────── */

typedef struct {
    int   fd;
    char  buf[1024];
    int   pos;
} stream_t;

static void stream_begin(stream_t *s, int fd, const char *ct)
{
    s->fd  = fd;
    s->pos = 0;
    char hdr[512];
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n", ct);
    send(fd, hdr, n, MSG_NOSIGNAL);
}

/* Flush buffered data as a chunk. */
static void stream_flush(stream_t *s)
{
    if (s->pos == 0) return;
    char hdr[16];
    int hn = snprintf(hdr, sizeof(hdr), "%x\r\n", s->pos);
    send(s->fd, hdr, hn, MSG_NOSIGNAL);
    send(s->fd, s->buf, s->pos, MSG_NOSIGNAL);
    send(s->fd, "\r\n", 2, MSG_NOSIGNAL);
    s->pos = 0;
}

/* Write len bytes (may flush and/or directly chunk large data). */
static void stream_write(stream_t *s, const char *data, int len)
{
    if (len <= 0) return;

    /* If it fits in the buffer, just copy. */
    if (s->pos + len <= (int)sizeof(s->buf)) {
        memcpy(s->buf + s->pos, data, len);
        s->pos += len;
        return;
    }

    /* Flush what we have first. */
    stream_flush(s);

    /* If still too big, send as its own chunk. */
    if (len > (int)sizeof(s->buf)) {
        char hdr[16];
        int hn = snprintf(hdr, sizeof(hdr), "%x\r\n", len);
        send(s->fd, hdr, hn, MSG_NOSIGNAL);
        send(s->fd, data, len, MSG_NOSIGNAL);
        send(s->fd, "\r\n", 2, MSG_NOSIGNAL);
        return;
    }

    memcpy(s->buf + s->pos, data, len);
    s->pos += len;
}

/* Format and write (via the buffer). */
static void stream_printf(stream_t *s, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char tmp[1536];
    (void)vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);
    size_t n = strlen(tmp);
    if (n > 0) stream_write(s, tmp, n);
}

/* Finish chunked response. */
static void stream_end(stream_t *s)
{
    stream_flush(s);
    send(s->fd, "0\r\n\r\n", 5, MSG_NOSIGNAL);
}

/* ─────────────────────────────────────────────
 *  GPIO helpers
 * ───────────────────────────────────────────── */

typedef struct {
    const char *label;
    int8_t      gpio;
} pin_entry_t;

#define MAX_DUT_PINS 64
static pin_entry_t s_dut_pins[MAX_DUT_PINS];
static int s_dut_pin_count;

static void collect_dut_pins(void)
{
    size_t count;
    const board_pin_t *pins = board_get_pins(&count);
    s_dut_pin_count = 0;
    for (size_t i = 0; i < count && s_dut_pin_count < MAX_DUT_PINS; i++) {
        const char *l = pins[i].label;
        if ((l[0] == 'T' || l[0] == 'B') && l[1] >= '1' && l[1] <= '9') {
            s_dut_pins[s_dut_pin_count].label = pins[i].label;
            s_dut_pins[s_dut_pin_count].gpio  = pins[i].gpio;
            s_dut_pin_count++;
        }
    }
}

static pin_entry_t *find_pin_by_label(const char *label)
{
    for (int i = 0; i < s_dut_pin_count; i++) {
        if (strcasecmp(s_dut_pins[i].label, label) == 0)
            return &s_dut_pins[i];
    }
    return NULL;
}

static int read_gpio_safe(int8_t gpio)
{
    if (gpio < 0) return -1;
    return gpio_get_level(gpio);
}

static bool config_pin_output(pin_entry_t *pin)
{
    if (pin->gpio < 0) return false;
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin->gpio,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg) == ESP_OK;
}

static bool config_pin_input(pin_entry_t *pin)
{
    if (pin->gpio < 0) return false;
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin->gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg) == ESP_OK;
}

/* ─────────────────────────────────────────────
 *  JSON helpers (small, keep buffer-based)
 * ───────────────────────────────────────────── */

/* ─────────────────────────────────────────────
 *  JSON helpers (small, keep buffer-based)
 * ───────────────────────────────────────────── */

typedef struct {
    char method[16];
    char url[256];
    int  content_length;
} http_request_t;

static bool parse_request(const char *buf, size_t len, http_request_t *req)
{
    memset(req, 0, sizeof(*req));
    const char *eol = (const char *)memchr(buf, '\n', len);
    if (!eol) return false;
    if (sscanf(buf, "%15s %255s", req->method, req->url) < 2) return false;
    const char *cl = strstr(buf, "Content-Length:");
    if (cl) sscanf(cl, "Content-Length: %d", &req->content_length);
    cl = strstr(buf, "content-length:");
    if (cl) sscanf(cl, "content-length: %d", &req->content_length);
    return true;
}

static const char *path_after(const char *url, const char *prefix)
{
    size_t plen = strlen(prefix);
    return (strncmp(url, prefix, plen) == 0) ? url + plen : NULL;
}

static void build_pins_json(char *buf, size_t buf_size)
{
    int off = 0;
    off += snprintf(buf + off, buf_size - off, "{\"pins\":[");
    for (int i = 0; i < s_dut_pin_count; i++) {
        int val = read_gpio_safe(s_dut_pins[i].gpio);
        off += snprintf(buf + off, buf_size - off,
            "%c{\"name\":\"%s\",\"gpio\":%d,\"value\":%d}",
            (i > 0) ? ',' : ' ',
            s_dut_pins[i].label, s_dut_pins[i].gpio, val);
        if ((size_t)off >= buf_size) break;
    }
    off += snprintf(buf + off, buf_size - off, "]}");
}

static void build_devices_json(char *buf, size_t buf_size)
{
    usbip_backend_device_t devs[CONFIG_USBIP_MAX_DEVICES + VIRTUAL_DEVICE_MAX];
    size_t count = usb_backend_get_devices(devs, sizeof(devs) / sizeof(devs[0]));
    int off = 0;
    off += snprintf(buf + off, buf_size - off, "{\"devices\":[");
    for (size_t i = 0; i < count; i++) {
        off += snprintf(buf + off, buf_size - off,
            "%c{\"busid\":\"%s\",\"vendor\":\"0x%04x\",\"product\":\"0x%04x\","
            "\"bcdDevice\":\"0x%04x\",\"speed\":%lu,\"class\":\"0x%02x\"}",
            (i > 0) ? ',' : ' ',
            devs[i].busid, devs[i].id_vendor, devs[i].id_product,
            devs[i].bcd_device, (unsigned long)devs[i].speed, devs[i].device_class);
        if ((size_t)off >= buf_size) break;
    }
    off += snprintf(buf + off, buf_size - off, "]}");
}

static const char *k_json_ct = "application/json; charset=utf-8";

static void send_json_ok(int fd, const char *body, size_t len)
{
    char hdr[512];
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n", k_json_ct, (unsigned)len);
    send(fd, hdr, n, MSG_NOSIGNAL);
    send(fd, body, len, MSG_NOSIGNAL);
}

static void http_404(int fd)
{
    send_json_ok(fd, "{\"error\":\"Not Found\"}", 20);
}

static void http_400(int fd, const char *msg)
{
    char body[256];
    int n = snprintf(body, sizeof(body), "{\"error\":\"%s\"}", msg);
    send_json_ok(fd, body, n);
}

static void http_405(int fd)
{
    send_json_ok(fd, "{\"error\":\"Method Not Allowed\"}", 28);
}

static void http_500(int fd)
{
    send_json_ok(fd, "{\"error\":\"Internal Server Error\"}", 30);
}

/* ─────────────────────────────────────────────
 *  Streaming HTML page builder
 * ───────────────────────────────────────────── */

static void stream_html_page(stream_t *s)
{
    stream_printf(s,
        "<!DOCTYPE html>\n"
        "<html lang=\"en\">\n"
        "<head>\n"
        "<meta charset=\"utf-8\">\n"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
        "<title>USB/IP Bridge &mdash; Status</title>\n"
        "<style>\n");

    stream_printf(s,
        "*{box-sizing:border-box;margin:0;padding:0}\n"
        "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;"
        "background:#1a1a2e;color:#eee;padding:16px;max-width:1200px;margin:0 auto}\n"
        "h1{color:#e94560;margin-bottom:4px;font-size:1.4rem}\n"
        "h2{background:#e94560;color:#0f3460;display:inline-block;"
        "padding:3px 12px;border-radius:4px;margin:12px 0 6px;font-size:0.95rem}\n"
        "p.sub{color:#888;margin-bottom:12px;font-size:0.8rem}\n"
        "table{width:100%%;border-collapse:collapse;margin-bottom:10px;font-size:0.8rem}\n"
        "th{background:#16213e;padding:6px 8px;text-align:left;font-weight:600;color:#e94560}\n"
        "td{padding:5px 8px;border-bottom:1px solid #16213e}\n"
        "tr:hover{background:#16213e}\n"
        ".hi{color:#4ade80;font-weight:bold}\n"
        ".lo{color:#f87171}\n");

    stream_printf(s,
        "button{background:#0f3460;color:#eee;border:1px solid #e94560;"
        "padding:2px 8px;border-radius:3px;cursor:pointer;font-size:0.75rem}\n"
        "button:hover{background:#e94560}\n"
        "select{background:#0f3460;color:#eee;border:1px solid #555;"
        "padding:1px 4px;border-radius:3px;font-size:0.75rem}\n"
        ".tabs{display:flex;gap:3px;margin-bottom:10px}\n"
        ".tabs button{flex:1;padding:6px;background:#16213e;"
        "border:1px solid #0f3460;border-radius:4px 4px 0 0}\n"
        ".tabs button.on{background:#e94560;border-color:#e94560}\n"
        ".sec{display:none}\n"
        ".sec.on{display:block}\n"
        "footer{text-align:center;color:#555;margin-top:20px;font-size:0.7rem}\n"
        "@media(max-width:600px){th,td{padding:3px 5px;font-size:0.7rem}}\n"
        "</style>\n"
        "</head>\n"
        "<body>\n");

    stream_printf(s,
        "<h1>\U0001f50c USB/IP Bridge</h1>\n"
        "<p class=\"sub\">Board: %s &mdash; %d DUT pins</p>\n"
        "<div class=\"tabs\">\n"
        "<button id=\"tp\" class=\"on\" onclick=\"st('p')\">GPIO Pins</button>\n"
        "<button id=\"tu\" onclick=\"st('u')\">USB Devices</button>\n"
        "</div>\n"
        "<div id=\"sp\" class=\"sec on\">\n"
        "<h2>DUT Header Pins (T1&ndash;T19 / B1&ndash;B20)</h2>\n"
        "<table><thead><tr><th>Pin</th><th>GPIO</th><th>Value</th>"
        "<th>Dir</th><th>Action</th></tr></thead><tbody>\n",
        board_get_name(), s_dut_pin_count);

    /* Stream each pin row */
    for (int i = 0; i < s_dut_pin_count; i++) {
        const pin_entry_t *pin = &s_dut_pins[i];
        int val = read_gpio_safe(pin->gpio);
        if (val < 0) val = 0;
        const char *cls = val ? "hi" : "lo";
        stream_printf(s,
            "<tr><td><strong>%s</strong></td><td>GPIO%d</td>"
            "<td class=\"%s\" id=\"v%s\">%s</td>"
            "<td><select id=\"d%s\" onchange=\"sd('%s')\">"
            "<option value=\"in\">IN</option><option value=\"out\">OUT</option>"
            "</select></td>"
            "<td><button onclick=\"tg('%s')\">Toggle</button></td></tr>\n",
            pin->label, pin->gpio,
            cls, pin->label, val ? "HIGH" : "LOW",
            pin->label, pin->label,
            pin->label);
    }

    stream_printf(s,
        "</tbody></table></div>\n"
        "<div id=\"su\" class=\"sec\">\n"
        "<h2>USB Devices</h2>\n"
        "<table><thead><tr><th>BusID</th><th>Vendor</th><th>Product</th>"
        "<th>Speed</th><th>Class</th></tr></thead><tbody>\n");

    /* Stream USB devices */
    {
        usbip_backend_device_t devs[CONFIG_USBIP_MAX_DEVICES + VIRTUAL_DEVICE_MAX];
        size_t dc = usb_backend_get_devices(devs, sizeof(devs) / sizeof(devs[0]));
        for (size_t i = 0; i < dc; i++) {
            const char *sp = "?";
            switch (devs[i].speed) {
                case 1: sp = "Low";  break;
                case 2: sp = "Full"; break;
                case 3: sp = "High"; break;
            }
            stream_printf(s,
                "<tr><td>%s</td><td>%04x</td><td>%04x</td>"
                "<td>%s</td><td>0x%02x</td></tr>\n",
                devs[i].busid, devs[i].id_vendor, devs[i].id_product,
                sp, devs[i].device_class);
        }
        if (dc == 0) {
            stream_printf(s,
                "<tr><td colspan=\"5\" style=\"text-align:center;color:#888\">"
                "No USB devices</td></tr>\n");
        }
    }

    stream_printf(s,
        "</tbody></table></div>\n"
        "<footer>USB/IP Bridge &mdash; <a href=\"/\" style=\"color:#555\">refresh</a></footer>\n"
        "<script>\n"
        "function st(n){['p','u'].forEach(function(x){"
        "document.getElementById('t'+x).className='';"
        "document.getElementById('s'+x).className='sec'});"
        "document.getElementById('t'+n).className='on';"
        "document.getElementById('s'+n).className='sec on'}\n"
        "function sd(p){var d=document.getElementById('d'+p).value;"
        "var x=new XMLHttpRequest();"
        "x.open('POST','/api/pins/'+p,true);"
        "x.setRequestHeader('Content-Type','application/json');"
        "x.send(JSON.stringify({direction:d}))}\n"
        "function tg(p){var x=new XMLHttpRequest();"
        "x.onload=function(){if(x.status==200){"
        "var r=JSON.parse(x.responseText);"
        "var e=document.getElementById('v'+p);"
        "e.textContent=r.value?'HIGH':'LOW';"
        "e.className=r.value?'hi':'lo'}};"
        "x.open('POST','/api/pins/'+p+'/toggle',true);x.send()}\n"
        "</script>\n"
        "</body></html>\n");
}

/* ─────────────────────────────────────────────
 *  Request routing
 * ───────────────────────────────────────────── */

static void handle_post_pin(int fd, const char *pin_name,
                            const char *body, size_t body_len)
{
    (void)body_len;
    pin_entry_t *pin = find_pin_by_label(pin_name);
    if (!pin) { http_404(fd); return; }

    int new_val = -1;
    char direction[8] = {0};

    const char *vk = strstr(body, "\"value\"");
    if (vk) {
        const char *c = strchr(vk, ':');
        if (c) {
            while (*c == ':' || *c == ' ') c++;
            if (*c == '0') new_val = 0;
            else if (*c == '1') new_val = 1;
        }
    }

    const char *dk = strstr(body, "\"direction\"");
    if (dk) {
        const char *c = strchr(dk, ':');
        if (c) {
            const char *q1 = strchr(c, '"');
            if (q1) {
                const char *q2 = strchr(q1 + 1, '"');
                if (q2 && (size_t)(q2 - q1 - 1) < sizeof(direction)) {
                    memcpy(direction, q1 + 1, q2 - q1 - 1);
                    direction[q2 - q1 - 1] = '\0';
                }
            }
        }
    }

    if (direction[0]) {
        if (strcmp(direction, "in") == 0) config_pin_input(pin);
        else if (strcmp(direction, "out") == 0) config_pin_output(pin);
    }
    if (new_val >= 0) gpio_set_level(pin->gpio, new_val);

    int level = read_gpio_safe(pin->gpio);
    if (level < 0) level = 0;
    char resp[128];
    int rlen = snprintf(resp, sizeof(resp),
        "{\"name\":\"%s\",\"gpio\":%d,\"value\":%d}",
        pin->label, pin->gpio, level);
    send_json_ok(fd, resp, rlen);
}

static void handle_connection(int fd)
{
    char *buf = malloc(HTTP_BUF_SIZE);
    if (!buf) { close(fd); return; }
    size_t total = 0;

    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (total < HTTP_BUF_SIZE) {
        int n = read(fd, buf + total, HTTP_BUF_SIZE - total);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            free(buf);
            return;
        }
        if (n == 0) break;
        total += (size_t)n;
        if (total >= 4 && memcmp(buf + total - 4, "\r\n\r\n", 4) == 0) break;
    }
    if (total == 0) { free(buf); return; }

    http_request_t req;
    if (!parse_request(buf, total, &req)) { free(buf); http_400(fd, "Bad request"); return; }

    /* Read body if present */
    char *body_buf = malloc(512);
    size_t body_read = 0;
    if (!body_buf) { free(buf); http_500(fd); return; }
    if (req.content_length > 0) {
        const char *start = strstr(buf, "\r\n\r\n");
        if (start) {
            start += 4;
            size_t already = buf + total - start;
            if (already > 0) {
                size_t copy = (already < 512) ? already : 512;
                memcpy(body_buf, start, copy);
                body_read = copy;
            }
            while (body_read < (size_t)req.content_length && body_read < 512) {
                int n = read(fd, body_buf + body_read, 512 - body_read);
                if (n <= 0) break;
                body_read += (size_t)n;
            }
        }
    }

    const char *url = req.url;

    /* GET / — stream the HTML page */
    if (strcmp(url, "/") == 0 && strcmp(req.method, "GET") == 0) {
        stream_t s;
        stream_begin(&s, fd, "text/html; charset=utf-8");
        stream_html_page(&s);
        stream_end(&s);
        free(body_buf); free(buf);
        return;
    }

    /* GET /api/pins */
    if (strcmp(url, "/api/pins") == 0 && strcmp(req.method, "GET") == 0) {
        char *resp = malloc(4096);
        if (!resp) { free(body_buf); free(buf); http_500(fd); return; }
        build_pins_json(resp, 4096);
        send_json_ok(fd, resp, strlen(resp));
        free(resp);
        free(body_buf); free(buf);
        return;
    }

    /* GET /api/devices */
    if (strcmp(url, "/api/devices") == 0 && strcmp(req.method, "GET") == 0) {
        char *resp = malloc(4096);
        if (!resp) { free(body_buf); free(buf); http_500(fd); return; }
        build_devices_json(resp, 4096);
        send_json_ok(fd, resp, strlen(resp));
        free(resp);
        free(body_buf); free(buf);
        return;
    }

    /* POST /api/pins/<name>[/toggle] */
    const char *rest = path_after(url, "/api/pins/");
    if (rest) {
        if (strcmp(req.method, "POST") != 0) { free(body_buf); free(buf); http_405(fd); return; }

        const char *slash = strchr(rest, '/');
        if (slash && strcmp(slash, "/toggle") == 0) {
            size_t nlen = slash - rest;
            if (nlen >= 32) { free(body_buf); free(buf); http_400(fd, "Name too long"); return; }
            char pname[32];
            memcpy(pname, rest, nlen);
            pname[nlen] = '\0';

            pin_entry_t *pin = find_pin_by_label(pname);
            if (!pin) { free(body_buf); free(buf); http_404(fd); return; }

            int level = read_gpio_safe(pin->gpio);
            if (level < 0) level = 0;
            gpio_set_level(pin->gpio, level ? 0 : 1);
            level = read_gpio_safe(pin->gpio);
            if (level < 0) level = 0;

            char resp[128];
            int rlen = snprintf(resp, sizeof(resp),
                "{\"name\":\"%s\",\"value\":%d}", pname, level);
            send_json_ok(fd, resp, rlen);
            free(body_buf); free(buf);
            return;
        }

        if (slash == NULL || *slash == '\0') {
            handle_post_pin(fd, rest, body_buf, body_read);
            free(body_buf); free(buf);
            return;
        }
    }

    free(body_buf); free(buf);
    http_404(fd);
}

/* ─────────────────────────────────────────────
 *  Server task
 * ───────────────────────────────────────────── */

static void http_server_task(void *arg)
{
    (void)arg;

    collect_dut_pins();
    ESP_LOGI(TAG, "Found %d DUT pins on board \"%s\"",
             s_dut_pin_count, board_get_name());

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) {
        ESP_LOGE(TAG, "socket(): errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(HTTP_PORT),
        .sin_addr   = { htonl(INADDR_ANY) },
    };

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind(%d): errno=%d", HTTP_PORT, errno);
        close(fd);
        vTaskDelete(NULL);
        return;
    }

    if (listen(fd, HTTP_MAX_CLIENTS) < 0) {
        ESP_LOGE(TAG, "listen(): errno=%d", errno);
        close(fd);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "HTTP server on port %d", HTTP_PORT);

    struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (true) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int cf = accept(fd, (struct sockaddr *)&peer, &plen);
        if (cf < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            ESP_LOGW(TAG, "accept(): errno=%d", errno);
            continue;
        }
        handle_connection(cf);
        close(cf);
    }
}

/* ─────────────────────────────────────────────
 *  Public API
 * ───────────────────────────────────────────── */

esp_err_t http_server_start(void)
{
    if (xTaskCreate(http_server_task, "http_server",
                    HTTP_TASK_STACK, NULL,
                    HTTP_TASK_PRIORITY, NULL) != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}
