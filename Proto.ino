#include <Arduino.h>
#include "soc/gpio_struct.h"
#include <WiFi.h>
#include <WebServer.h>


#ifndef WIFI_SSID
#define WIFI_SSID "INSERT YOURS"
#endif
#ifndef WIFI_PASS
#define WIFI_PASS "INSERT YOURS"
#endif

static const char *HOSTNAME = "arm6";

#define NUM_MOTORS 6

static const uint32_t TICK_HZ = 50000;
static const uint32_t MIN_TICKS_PER_STEP = 2;
static const uint32_t MAX_TICKS_PER_STEP = TICK_HZ; 

struct PinPair { uint8_t step; uint8_t dir; };
static const PinPair PINS[NUM_MOTORS] = {
    {13, 14},  // M0
    {4, 5},  // M1
    {18, 19},  // M2
    {21, 22},  // M3
    {23, 25},  // M4
    {26, 27},  // M5
};

struct Motor {
    uint32_t stepMask;
    uint8_t  dirPin;

    volatile bool     enabled;     
    volatile bool     dirCW;
    volatile uint32_t ticksPerStep;
    volatile uint32_t tickCounter;
    volatile bool     pulseHigh;
    volatile int32_t  stepsRemaining;
    volatile int32_t  position;       
};

static Motor motors[NUM_MOTORS];
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static hw_timer_t *stepTimer = nullptr;
static WebServer server(80);

void IRAM_ATTR onStepTick() {
    portENTER_CRITICAL_ISR(&mux);

    for (int i = 0; i < NUM_MOTORS; i++) {
        Motor &m = motors[i];

        if (m.pulseHigh) {
            GPIO.out_w1tc = m.stepMask;
            m.pulseHigh = false;
            continue;
        }

        if (!m.enabled) continue;
        if (m.stepsRemaining == 0) continue;

        if (++m.tickCounter >= m.ticksPerStep) {
            m.tickCounter = 0;
            GPIO.out_w1ts = m.stepMask;
            m.pulseHigh = true;
            m.position += m.dirCW ? 1 : -1;
            if (m.stepsRemaining > 0) m.stepsRemaining--;
        }
    }

    portEXIT_CRITICAL_ISR(&mux);
}

static uint32_t spsToTicks(float stepsPerSec) {
    if (stepsPerSec <= 0.0f) return MAX_TICKS_PER_STEP;
    uint32_t t = (uint32_t)((float)TICK_HZ / stepsPerSec + 0.5f);
    if (t < MIN_TICKS_PER_STEP) t = MIN_TICKS_PER_STEP;
    if (t > MAX_TICKS_PER_STEP) t = MAX_TICKS_PER_STEP;
    return t;
}

static void setDirection(int id, bool cw) {
    Motor &m = motors[id];
    if (m.dirCW == cw) return;

    portENTER_CRITICAL(&mux);
    int32_t saved = m.stepsRemaining;
    m.stepsRemaining = 0;
    portEXIT_CRITICAL(&mux);

    delayMicroseconds(50);           
    digitalWrite(m.dirPin, cw ? HIGH : LOW);
    delayMicroseconds(5);            

    portENTER_CRITICAL(&mux);
    m.dirCW = cw;
    m.tickCounter = 0;
    m.stepsRemaining = saved;
    portEXIT_CRITICAL(&mux);
}

static void stopMotor(int id) {
    portENTER_CRITICAL(&mux);
    motors[id].stepsRemaining = 0;
    motors[id].tickCounter = 0;
    portEXIT_CRITICAL(&mux);
}

static void stopAll() {
    for (int i = 0; i < NUM_MOTORS; i++) stopMotor(i);
}

// WEB UI, taken from Claude
static const char PAGE_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>6-Axis Control</title>
<style>
:root{--bg:#14161a;--card:#1e2228;--line:#2e343d;--fg:#e6e8eb;--dim:#8b939e;--go:#3fa66b;--stop:#c04b4b}
*{box-sizing:border-box}
body{margin:0;padding:12px;background:var(--bg);color:var(--fg);
     font:14px/1.4 ui-monospace,SFMono-Regular,Menlo,monospace}
h1{font-size:16px;margin:0 0 4px}
.sub{color:var(--dim);font-size:12px;margin-bottom:12px}
.grid{display:grid;gap:10px;grid-template-columns:repeat(auto-fill,minmax(280px,1fr))}
.card{background:var(--card);border:1px solid var(--line);border-radius:6px;padding:10px}
.hdr{display:flex;justify-content:space-between;align-items:center;margin-bottom:8px}
.name{font-weight:600}
.pos{color:var(--dim);font-size:12px}
.row{display:flex;gap:6px;align-items:center;margin-bottom:6px}
label{color:var(--dim);font-size:12px;width:52px;flex:none}
input[type=number]{flex:1;min-width:0;background:#0e1013;color:var(--fg);
     border:1px solid var(--line);border-radius:4px;padding:5px}
button{background:#2a3039;color:var(--fg);border:1px solid var(--line);
     border-radius:4px;padding:6px 10px;cursor:pointer;font:inherit;font-size:12px}
button:hover{background:#343b45}
button.on{background:var(--go);border-color:var(--go);color:#fff}
button.off{background:var(--stop);border-color:var(--stop);color:#fff}
.seg{display:flex;flex:1}
.seg button{flex:1;border-radius:0}
.seg button:first-child{border-radius:4px 0 0 4px}
.seg button:last-child{border-radius:0 4px 4px 0;border-left:none}
.estop{width:100%;padding:12px;margin-top:14px;background:var(--stop);
     border-color:var(--stop);color:#fff;font-size:15px;font-weight:600}
.dot{display:inline-block;width:7px;height:7px;border-radius:50%;
     background:#4a525c;margin-right:5px;vertical-align:middle}
.dot.run{background:var(--go)}
</style></head><body>
<h1>6-Axis Motor Control</h1>
<div class="sub" id="status">connecting...</div>
<div class="grid" id="grid"></div>
<button class="estop" onclick="api('/api/stopall')">STOP ALL</button>
<script>
const N=6; let st=[];
function el(h){const d=document.createElement('div');d.innerHTML=h.trim();return d.firstChild;}
function build(){
  const g=document.getElementById('grid'); g.innerHTML='';
  for(let i=0;i<N;i++){
    g.appendChild(el(`
    <div class="card">
      <div class="hdr">
        <span class="name"><span class="dot" id="d${i}"></span>M${i}</span>
        <span class="pos" id="p${i}">0</span>
      </div>
      <div class="row">
        <label>enable</label>
        <div class="seg">
          <button id="en${i}0" onclick="set(${i},{en:0})">OFF</button>
          <button id="en${i}1" onclick="set(${i},{en:1})">ON</button>
        </div>
      </div>
      <div class="row">
        <label>dir</label>
        <div class="seg">
          <button id="dr${i}1" onclick="set(${i},{dir:1})">CW</button>
          <button id="dr${i}0" onclick="set(${i},{dir:0})">CCW</button>
        </div>
      </div>
      <div class="row">
        <label>steps/s</label>
        <input type="number" id="sp${i}" value="400" min="1" max="25000"
               onchange="set(${i},{sps:this.value})">
      </div>
      <div class="row">
        <label>jog</label>
        <div class="seg">
          <button onclick="api('/api/jog?id=${i}&run=1')">RUN</button>
          <button onclick="api('/api/jog?id=${i}&run=0')">STOP</button>
        </div>
      </div>
      <div class="row">
        <label>move</label>
        <input type="number" id="mv${i}" value="200" min="1">
        <button onclick="api('/api/move?id=${i}&steps='+
          document.getElementById('mv${i}').value)">GO</button>
      </div>
    </div>`));
  }
}
async function api(u){ try{ const r=await fetch(u); st=await r.json(); paint(); }
  catch(e){ document.getElementById('status').textContent='connection lost'; } }
function set(i,o){
  let q='/api/set?id='+i;
  if(o.en!==undefined) q+='&en='+o.en;
  if(o.dir!==undefined) q+='&dir='+o.dir;
  if(o.sps!==undefined) q+='&sps='+o.sps;
  api(q);
}
function paint(){
  document.getElementById('status').textContent =
    'connected  |  tick 50 kHz  |  max 25000 steps/s';
  for(let i=0;i<N;i++){
    const m=st[i]; if(!m) continue;
    document.getElementById('p'+i).textContent = m.pos;
    document.getElementById('d'+i).className = 'dot'+(m.running?' run':'');
    document.getElementById('en'+i+'1').className = m.en?'on':'';
    document.getElementById('en'+i+'0').className = m.en?'':'off';
    document.getElementById('dr'+i+'1').className = m.cw?'on':'';
    document.getElementById('dr'+i+'0').className = m.cw?'':'on';
    const sp=document.getElementById('sp'+i);
    if(document.activeElement!==sp) sp.value = m.sps;
  }
}
build(); api('/api/state'); setInterval(()=>api('/api/state'),500);
</script></body></html>
)rawliteral";

static void sendState() {
    String j = "[";
    for (int i = 0; i < NUM_MOTORS; i++) {
        portENTER_CRITICAL(&mux);
        bool en = motors[i].enabled;
        bool cw = motors[i].dirCW;
        uint32_t tps = motors[i].ticksPerStep;
        int32_t rem = motors[i].stepsRemaining;
        int32_t pos = motors[i].position;
        portEXIT_CRITICAL(&mux);

        if (i) j += ",";
        j += "{\"en\":";      j += en ? 1 : 0;
        j += ",\"cw\":";      j += cw ? 1 : 0;
        j += ",\"sps\":";     j += (uint32_t)((float)TICK_HZ / (float)tps + 0.5f);
        j += ",\"running\":"; j += (en && rem != 0) ? 1 : 0;
        j += ",\"pos\":";     j += pos;
        j += "}";
    }
    j += "]";
    server.send(200, "application/json", j);
}

static int argId() {
    if (!server.hasArg("id")) return -1;
    int id = server.arg("id").toInt();
    return (id >= 0 && id < NUM_MOTORS) ? id : -1;
}

static void handleSet() {
    int id = argId();
    if (id < 0) { server.send(400, "text/plain", "bad id"); return; }

    if (server.hasArg("dir"))
        setDirection(id, server.arg("dir").toInt() != 0);

    if (server.hasArg("sps")) {
        uint32_t t = spsToTicks(server.arg("sps").toFloat());
        portENTER_CRITICAL(&mux);
        motors[id].ticksPerStep = t;
        if (motors[id].tickCounter >= t) motors[id].tickCounter = 0;
        portEXIT_CRITICAL(&mux);
    }

    if (server.hasArg("en")) {
        bool en = server.arg("en").toInt() != 0;
        portENTER_CRITICAL(&mux);
        motors[id].enabled = en;
        if (!en) { motors[id].stepsRemaining = 0; motors[id].tickCounter = 0; }
        portEXIT_CRITICAL(&mux);
    }

    sendState();
}

static void handleJog() {
    int id = argId();
    if (id < 0) { server.send(400, "text/plain", "bad id"); return; }
    bool run = server.hasArg("run") && server.arg("run").toInt() != 0;

    portENTER_CRITICAL(&mux);
    motors[id].stepsRemaining = run ? -1 : 0;
    motors[id].tickCounter = 0;
    portEXIT_CRITICAL(&mux);

    sendState();
}

static void handleMove() {
    int id = argId();
    if (id < 0) { server.send(400, "text/plain", "bad id"); return; }
    int32_t n = server.hasArg("steps") ? server.arg("steps").toInt() : 0;
    if (n < 0) n = 0;

    portENTER_CRITICAL(&mux);
    motors[id].stepsRemaining = n;
    motors[id].tickCounter = 0;
    portEXIT_CRITICAL(&mux);

    sendState();
}

static void handleStopAll() {
    stopAll();
    sendState();
}

void setup() {
    Serial.begin(115200);
    delay(200);

    for (int i = 0; i < NUM_MOTORS; i++) {
        pinMode(PINS[i].step, OUTPUT);
        pinMode(PINS[i].dir, OUTPUT);
        digitalWrite(PINS[i].step, LOW);
        digitalWrite(PINS[i].dir, HIGH);

        motors[i].stepMask      = 1UL << PINS[i].step;
        motors[i].dirPin        = PINS[i].dir;
        motors[i].enabled       = false;
        motors[i].dirCW         = true;
        motors[i].ticksPerStep  = spsToTicks(400);
        motors[i].tickCounter   = 0;
        motors[i].pulseHigh     = false;
        motors[i].stepsRemaining= 0;
        motors[i].position      = 0;
    }

    WiFi.mode(WIFI_STA);
    WiFi.setHostname(HOSTNAME);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("connecting");
    while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print("."); }
    Serial.printf("\nready at http://%s/\n", WiFi.localIP().toString().c_str());

    server.on("/", HTTP_GET, []() {
        server.send_P(200, "text/html", PAGE_HTML);
    });
    server.on("/api/state",   HTTP_GET, sendState);
    server.on("/api/set",     HTTP_GET, handleSet);
    server.on("/api/jog",     HTTP_GET, handleJog);
    server.on("/api/move",    HTTP_GET, handleMove);
    server.on("/api/stopall", HTTP_GET, handleStopAll);
    server.begin();

#if ESP_ARDUINO_VERSION_MAJOR >= 3
    stepTimer = timerBegin(TICK_HZ);
    timerAttachInterrupt(stepTimer, &onStepTick);
    timerAlarm(stepTimer, 1, true, 0);
#else
    stepTimer = timerBegin(0, 80, true);
    timerAttachInterrupt(stepTimer, &onStepTick, true);
    timerAlarmWrite(stepTimer, 1000000UL / TICK_HZ, true);
    timerAlarmEnable(stepTimer);
#endif
}

void loop() {
    server.handleClient();

    static bool wasUp = true;
    bool up = (WiFi.status() == WL_CONNECTED);
    if (wasUp && !up) { stopAll(); Serial.println("wifi lost - stopped all"); }
    wasUp = up;
}