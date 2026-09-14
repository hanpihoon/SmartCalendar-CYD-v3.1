#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <SPI.h>
#include <SPIFFS.h>
#include <Update.h>
#include <time.h>
#include <vector>
#include <algorithm>

// ============================================================
// Smart Calendar Pro - ESP32-2432S028R / CYD 2.8"
// Target: classic CYD 2.8 portrait 240x320, ILI9341, XPT2046, ESP32-WROOM-32
// ============================================================

// ---------- CYD pinout ----------
static const int TFT_MISO=12, TFT_MOSI=13, TFT_SCLK=14, TFT_CS=15, TFT_DC=2, TFT_BL=21;
static const int TOUCH_CLK=25, TOUCH_MOSI=32, TOUCH_CS=33, TOUCH_IRQ=36, TOUCH_MISO=39;
static const int BOOT_BTN=0;

SPIClass tftSPI(HSPI);
SPIClass touchSPI(VSPI);
Preferences prefs;
WebServer server(80);
DNSServer dns;

// ---------- Theme ----------
uint16_t rgb565(uint8_t r,uint8_t g,uint8_t b){ return ((r&0xF8)<<8)|((g&0xFC)<<3)|(b>>3); }
const uint16_t C_BG      = 0x0841;
const uint16_t C_SURFACE = 0x10A2;
const uint16_t C_SURF2   = 0x18E3;
const uint16_t C_BORDER  = 0x2945;
const uint16_t C_TEXT    = 0xEF7D;
const uint16_t C_MUTED   = 0x8410;
const uint16_t C_ACCENT  = 0xFD20;
const uint16_t C_GOOGLE  = 0x3D7F;
const uint16_t C_OUTLOOK = 0x04BF;
const uint16_t C_GREEN   = 0x4E69;
const uint16_t C_RED     = 0xF166;
const uint16_t C_YELLOW  = 0xFDE0;
const uint16_t C_WHITE   = 0xFFFF;
const uint16_t C_BLACK   = 0x0000;

// ---------- Config ----------
struct Config {
  String name = "Smart Calendar";
  String wifiSsid;
  String wifiPass;
  String googleIcs;
  String outlookIcs;
  int utcOffsetMinutes = 420; // UTC+7
  int refreshMinutes = 10;
  int brightness = 85;
  float latitude = 21.0285;
  float longitude = 105.8542;
  bool showWeather = true;
  bool hour24 = true;
  // XPT2046 calibration (editable from portal for clone variance)
  int txMin=260, txMax=3850, tyMin=260, tyMax=3850;
  bool touchSwap=true, touchInvX=false, touchInvY=true;
} cfg;

struct Event {
  time_t start=0;
  time_t end=0;
  String title;
  String location;
  String source;
  bool allDay=false;
};
std::vector<Event> events;

struct WeatherState {
  bool valid=false;
  float temp=0;
  int code=0;
  unsigned long lastUpdate=0;
} weather;

enum Screen { SCREEN_HOME, SCREEN_MONTH, SCREEN_SETTINGS };
Screen activeScreen = SCREEN_HOME;
bool portalMode=false;
bool serverStarted=false;
bool timeValid=false;
bool syncOk=false;
String syncMessage="Not synced";
unsigned long lastSync=0, lastWeather=0, lastDraw=0, lastTouch=0;
int selectedMonthOffset=0;

// ============================================================
// Minimal ILI9341 graphics - no external display library needed
// ============================================================
#define CMD 0
#define DATA 1
static inline void dc(bool d){ digitalWrite(TFT_DC,d); }
void tftWrite8(uint8_t v, bool isData){ dc(isData); digitalWrite(TFT_CS,LOW); tftSPI.transfer(v); digitalWrite(TFT_CS,HIGH); }
void tftCmd(uint8_t c){ tftWrite8(c,CMD); }
void tftData(uint8_t d){ tftWrite8(d,DATA); }
void tftData16(uint16_t d){ dc(DATA); digitalWrite(TFT_CS,LOW); tftSPI.transfer(d>>8); tftSPI.transfer(d); digitalWrite(TFT_CS,HIGH); }
void setAddr(int x0,int y0,int x1,int y1){
  tftCmd(0x2A); tftData16(x0); tftData16(x1);
  tftCmd(0x2B); tftData16(y0); tftData16(y1);
  tftCmd(0x2C);
}
void fillRect(int x,int y,int w,int h,uint16_t c){
  if(w<=0||h<=0||x>=240||y>=320||x+w<=0||y+h<=0)return;
  if(x<0){w+=x;x=0;} if(y<0){h+=y;y=0;} if(x+w>240)w=240-x; if(y+h>320)h=320-y;
  setAddr(x,y,x+w-1,y+h-1); dc(DATA); digitalWrite(TFT_CS,LOW);
  for(int i=0;i<w*h;i++){tftSPI.transfer(c>>8);tftSPI.transfer(c);} digitalWrite(TFT_CS,HIGH);
}
void fillScreen(uint16_t c){ fillRect(0,0,240,320,c); }
void drawPixel(int x,int y,uint16_t c){ if(x<0||x>=240||y<0||y>=320)return; setAddr(x,y,x,y); dc(DATA); digitalWrite(TFT_CS,LOW);tftSPI.transfer(c>>8);tftSPI.transfer(c);digitalWrite(TFT_CS,HIGH); }
void drawHLine(int x,int y,int w,uint16_t c){ fillRect(x,y,w,1,c); }
void drawVLine(int x,int y,int h,uint16_t c){ fillRect(x,y,1,h,c); }
void drawLine(int x0,int y0,int x1,int y1,uint16_t c){
  int dx=abs(x1-x0), sx=x0<x1?1:-1, dy=-abs(y1-y0), sy=y0<y1?1:-1, err=dx+dy;
  while(true){drawPixel(x0,y0,c);if(x0==x1&&y0==y1)break;int e2=2*err;if(e2>=dy){err+=dy;x0+=sx;}if(e2<=dx){err+=dx;y0+=sy;}}
}
void fillCircle(int cx,int cy,int r,uint16_t c){ for(int y=-r;y<=r;y++){int xx=(int)sqrt((float)(r*r-y*y));fillRect(cx-xx,cy+y,xx*2+1,1,c);} }
void drawCircle(int cx,int cy,int r,uint16_t c){int x=r,y=0,err=0;while(x>=y){drawPixel(cx+x,cy+y,c);drawPixel(cx+y,cy+x,c);drawPixel(cx-y,cy+x,c);drawPixel(cx-x,cy+y,c);drawPixel(cx-x,cy-y,c);drawPixel(cx-y,cy-x,c);drawPixel(cx+y,cy-x,c);drawPixel(cx+x,cy-y,c);y++;if(err<=0)err+=2*y+1;if(err>0){x--;err-=2*x+1;}}}
void fillRoundRect(int x,int y,int w,int h,int r,uint16_t c){
  if(r<1){fillRect(x,y,w,h,c);return;} r=min(r,min(w,h)/2);
  fillRect(x+r,y,w-2*r,h,c); fillRect(x,y+r,r,h-2*r,c); fillRect(x+w-r,y+r,r,h-2*r,c);
  for(int yy=0;yy<r;yy++){int xx=(int)sqrt((float)(r*r-(r-yy)*(r-yy)));fillRect(x+r-xx,y+yy,w-2*r+2*xx,1,c);fillRect(x+r-xx,y+h-1-yy,w-2*r+2*xx,1,c);}
}
void drawRoundRect(int x,int y,int w,int h,int r,uint16_t c){
  drawHLine(x+r,y,w-2*r,c);drawHLine(x+r,y+h-1,w-2*r,c);drawVLine(x,y+r,h-2*r,c);drawVLine(x+w-1,y+r,h-2*r,c);
  for(int a=0;a<=90;a+=4){float rad=a*PI/180.0;int xx=(int)(cos(rad)*r),yy=(int)(sin(rad)*r);drawPixel(x+r-xx,y+r-yy,c);drawPixel(x+w-1-r+xx,y+r-yy,c);drawPixel(x+r-xx,y+h-1-r+yy,c);drawPixel(x+w-1-r+xx,y+h-1-r+yy,c);}
}

void setBacklight(int percent){ percent=constrain(percent,5,100); ledcWrite(0,map(percent,0,100,0,255)); }

void tftInit(){
  pinMode(TFT_CS,OUTPUT); pinMode(TFT_DC,OUTPUT); pinMode(TFT_BL,OUTPUT); digitalWrite(TFT_CS,HIGH);
  tftSPI.begin(TFT_SCLK,TFT_MISO,TFT_MOSI,TFT_CS); tftSPI.setFrequency(40000000);
  delay(50); tftCmd(0x01); delay(120); tftCmd(0x28);
  tftCmd(0x3A); tftData(0x55); // RGB565
  tftCmd(0x36); tftData(0x48); // portrait upright, BGR
  tftCmd(0x11); delay(120); tftCmd(0x29);
  ledcSetup(0,5000,8); ledcAttachPin(TFT_BL,0); setBacklight(cfg.brightness);
}

// ---------- Compact 5x7 font ----------
const uint8_t font5x7[][5] PROGMEM = {
{0,0,0,0,0},{0,0,0x5f,0,0},{0,7,0,7,0},{0x14,0x7f,0x14,0x7f,0x14},{0x24,0x2a,0x7f,0x2a,0x12},{0x23,0x13,8,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0,5,3,0,0},{0,0x1c,0x22,0x41,0},{0,0x41,0x22,0x1c,0},{0x14,8,0x3e,8,0x14},{8,8,0x3e,8,8},{0,0x50,0x30,0,0},{8,8,8,8,8},{0,0x60,0x60,0,0},{0x20,0x10,8,4,2},
{0x3e,0x51,0x49,0x45,0x3e},{0,0x42,0x7f,0x40,0},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4b,0x31},{0x18,0x14,0x12,0x7f,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3c,0x4a,0x49,0x49,0x30},{1,0x71,9,5,3},{0x36,0x49,0x49,0x49,0x36},{6,0x49,0x49,0x29,0x1e},{0,0x36,0x36,0,0},{0,0x56,0x36,0,0},{8,0x14,0x22,0x41,0},{0x14,0x14,0x14,0x14,0x14},{0,0x41,0x22,0x14,8},{2,1,0x51,9,6},{0x32,0x49,0x79,0x41,0x3e},
{0x7e,0x11,0x11,0x11,0x7e},{0x7f,0x49,0x49,0x49,0x36},{0x3e,0x41,0x41,0x41,0x22},{0x7f,0x41,0x41,0x22,0x1c},{0x7f,0x49,0x49,0x49,0x41},{0x7f,9,9,9,1},{0x3e,0x41,0x49,0x49,0x7a},{0x7f,8,8,8,0x7f},{0,0x41,0x7f,0x41,0},{0x20,0x40,0x41,0x3f,1},{0x7f,8,0x14,0x22,0x41},{0x7f,0x40,0x40,0x40,0x40},{0x7f,2,0x0c,2,0x7f},{0x7f,4,8,0x10,0x7f},{0x3e,0x41,0x41,0x41,0x3e},{0x7f,9,9,9,6},{0x3e,0x41,0x51,0x21,0x5e},{0x7f,9,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},{1,1,0x7f,1,1},{0x3f,0x40,0x40,0x40,0x3f},{0x1f,0x20,0x40,0x20,0x1f},{0x3f,0x40,0x38,0x40,0x3f},{0x63,0x14,8,0x14,0x63},{7,8,0x70,8,7},{0x61,0x51,0x49,0x45,0x43}
};
void drawChar(int x,int y,char ch,uint16_t fg,uint16_t bg,int s=1){
  if(ch>='a'&&ch<='z')ch-=32; if(ch<32||ch>90)ch='?'; uint8_t idx=ch-32;
  for(int col=0;col<5;col++){uint8_t bits=pgm_read_byte(&font5x7[idx][col]);for(int row=0;row<7;row++)fillRect(x+col*s,y+row*s,s,s,(bits&(1<<row))?fg:bg);} fillRect(x+5*s,y,s,7*s,bg);
}
String asciiSafe(String s){
  s.replace("á","a");s.replace("à","a");s.replace("ả","a");s.replace("ã","a");s.replace("ạ","a");s.replace("ă","a");s.replace("â","a");
  s.replace("é","e");s.replace("è","e");s.replace("ê","e");s.replace("í","i");s.replace("ì","i");s.replace("ó","o");s.replace("ò","o");s.replace("ô","o");s.replace("ơ","o");
  s.replace("ú","u");s.replace("ù","u");s.replace("ư","u");s.replace("ý","y");s.replace("đ","d");s.replace("Đ","D");
  String out=""; for(size_t i=0;i<s.length();i++){uint8_t c=s[i]; if(c<128)out+=(char)c;} return out;
}
void drawText(int x,int y,String t,uint16_t fg,uint16_t bg,int s=1){t=asciiSafe(t);for(size_t i=0;i<t.length();i++)drawChar(x+i*6*s,y,t[i],fg,bg,s);}
int textW(String t,int s=1){return (int)asciiSafe(t).length()*6*s;}
void drawTextRight(int xRight,int y,String t,uint16_t fg,uint16_t bg,int s=1){drawText(xRight-textW(t,s),y,t,fg,bg,s);}

// ============================================================
// XPT2046 touch
// ============================================================
void touchInit(){
  pinMode(TOUCH_CS,OUTPUT); digitalWrite(TOUCH_CS,HIGH); pinMode(TOUCH_IRQ,INPUT);
  touchSPI.begin(TOUCH_CLK,TOUCH_MISO,TOUCH_MOSI,TOUCH_CS);
}
uint16_t touchReadAxis(uint8_t cmd){
  digitalWrite(TOUCH_CS,LOW); touchSPI.beginTransaction(SPISettings(2000000,MSBFIRST,SPI_MODE0));
  touchSPI.transfer(cmd); uint16_t v=((uint16_t)touchSPI.transfer(0)<<8)|touchSPI.transfer(0);
  touchSPI.endTransaction(); digitalWrite(TOUCH_CS,HIGH); return v>>3;
}
bool readTouch(int &sx,int &sy){
  if(digitalRead(TOUCH_IRQ)==HIGH)return false;
  uint32_t rx=0,ry=0; for(int i=0;i<5;i++){rx+=touchReadAxis(0xD0);ry+=touchReadAxis(0x90);} rx/=5;ry/=5;
  int x=map((int)rx,cfg.txMin,cfg.txMax,0,239); int y=map((int)ry,cfg.tyMin,cfg.tyMax,0,319);
  if(cfg.touchSwap){ x=map((int)ry,cfg.tyMin,cfg.tyMax,0,239); y=map((int)rx,cfg.txMin,cfg.txMax,0,319); }
  if(cfg.touchInvX)x=239-x; if(cfg.touchInvY)y=319-y;
  sx=constrain(x,0,239); sy=constrain(y,0,319); return true;
}

// ============================================================
// Persistent configuration
// ============================================================
void loadConfig(){
  prefs.begin("smartcal",true);
  cfg.name=prefs.getString("name","Smart Calendar"); cfg.wifiSsid=prefs.getString("ssid",""); cfg.wifiPass=prefs.getString("pass","");
  cfg.googleIcs=prefs.getString("gics",""); cfg.outlookIcs=prefs.getString("oics","");
  cfg.utcOffsetMinutes=prefs.getInt("utcmin",420); cfg.refreshMinutes=prefs.getInt("refresh",10); cfg.brightness=prefs.getInt("bright",85);
  cfg.latitude=prefs.getFloat("lat",21.0285); cfg.longitude=prefs.getFloat("lon",105.8542); cfg.showWeather=prefs.getBool("weather",true); cfg.hour24=prefs.getBool("hour24",true);
  cfg.txMin=prefs.getInt("txmin",260);cfg.txMax=prefs.getInt("txmax",3850);cfg.tyMin=prefs.getInt("tymin",260);cfg.tyMax=prefs.getInt("tymax",3850);
  cfg.touchSwap=prefs.getBool("tswap",false);cfg.touchInvX=prefs.getBool("tinvx",false);cfg.touchInvY=prefs.getBool("tinvy",false);
  prefs.end();
}
void saveConfig(){
  prefs.begin("smartcal",false);
  prefs.putString("name",cfg.name);prefs.putString("ssid",cfg.wifiSsid);prefs.putString("pass",cfg.wifiPass);prefs.putString("gics",cfg.googleIcs);prefs.putString("oics",cfg.outlookIcs);
  prefs.putInt("utcmin",cfg.utcOffsetMinutes);prefs.putInt("refresh",cfg.refreshMinutes);prefs.putInt("bright",cfg.brightness);prefs.putFloat("lat",cfg.latitude);prefs.putFloat("lon",cfg.longitude);
  prefs.putBool("weather",cfg.showWeather);prefs.putBool("hour24",cfg.hour24);prefs.putInt("txmin",cfg.txMin);prefs.putInt("txmax",cfg.txMax);prefs.putInt("tymin",cfg.tyMin);prefs.putInt("tymax",cfg.tyMax);
  prefs.putBool("tswap",cfg.touchSwap);prefs.putBool("tinvx",cfg.touchInvX);prefs.putBool("tinvy",cfg.touchInvY);prefs.end();
}

String htmlEscape(String s){s.replace("&","&amp;");s.replace("\"","&quot;");s.replace("<","&lt;");s.replace(">","&gt;");return s;}
String checked(bool v){return v?" checked":"";}
String setupPage(){
  String h=R"HTML(<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1"><title>Smart Calendar</title><style>
  *{box-sizing:border-box}body{margin:0;background:#0d1117;color:#f1f5f9;font-family:Inter,system-ui,Arial}.wrap{max-width:760px;margin:auto;padding:28px 18px 60px}.hero{padding:24px;border:1px solid #28303b;background:linear-gradient(135deg,#161b22,#10151c);border-radius:22px;margin-bottom:18px}.logo{display:inline-flex;align-items:center;gap:10px;font-weight:800;font-size:22px}.dot{width:14px;height:14px;border-radius:50%;background:#f58220;box-shadow:0 0 18px #f58220}.sub{color:#94a3b8;margin-top:8px}.card{background:#141a22;border:1px solid #28303b;border-radius:18px;padding:18px;margin:14px 0}.card h3{margin:0 0 14px;font-size:15px;color:#f8fafc}label{display:block;color:#cbd5e1;font-size:13px;margin:12px 0 6px}input{width:100%;padding:12px;border-radius:11px;border:1px solid #334155;background:#0f141b;color:white;outline:none}input:focus{border-color:#f58220}.row{display:grid;grid-template-columns:1fr 1fr;gap:12px}.check{display:flex;gap:9px;align-items:center;margin-top:12px}.check input{width:auto}.hint{font-size:12px;color:#7f8c9d;margin-top:6px;line-height:1.45}.btn{display:inline-block;background:#f58220;border:0;color:#fff;font-weight:800;padding:13px 18px;border-radius:12px;cursor:pointer}.btn2{background:#243140}.status{font-size:12px;color:#93c5fd}.foot{color:#64748b;font-size:12px;text-align:center;margin-top:24px}@media(max-width:560px){.row{grid-template-columns:1fr}.wrap{padding:16px 12px 40px}}</style></head><body><div class=wrap>
  <div class=hero><div class=logo><span class=dot></span> Smart Calendar Pro</div><div class=sub>ESP32-2432S028R · Google + Outlook · Local-first setup</div></div><form method=POST action=/save>
  <div class=card><h3>DEVICE & WIFI</h3><label>Device name</label><input name=name value=")HTML"+htmlEscape(cfg.name)+R"HTML("><label>Wi-Fi SSID</label><input name=ssid value=")HTML"+htmlEscape(cfg.wifiSsid)+R"HTML("><label>Wi-Fi password</label><input type=password name=pass value=")HTML"+htmlEscape(cfg.wifiPass)+R"HTML("><div class=hint>Your Google/Microsoft passwords are never stored on this device.</div></div>
  <div class=card><h3>CALENDAR SOURCES</h3><label>Google ICS URL (advanced / optional)</label><input name=gics value=")HTML"+htmlEscape(cfg.googleIcs)+R"HTML("><div class=hint>Google Calendar → Settings → Integrate calendar → Secret address in iCal format.</div><label>Outlook ICS URL (advanced / optional)</label><input name=oics value=")HTML"+htmlEscape(cfg.outlookIcs)+R"HTML("><div class=hint>Outlook → Settings → Shared calendars → Publish a calendar → ICS.</div></div>
  <div class=card><h3>DISPLAY & SYNC</h3><div class=row><div><label>UTC offset minutes</label><input type=number min=-720 max=840 name=utc value=")HTML"+String(cfg.utcOffsetMinutes)+R"HTML("></div><div><label>Refresh interval (min)</label><input type=number min=5 max=120 name=refresh value=")HTML"+String(cfg.refreshMinutes)+R"HTML("></div></div><div class=row><div><label>Brightness %</label><input type=number min=5 max=100 name=bright value=")HTML"+String(cfg.brightness)+R"HTML("></div><div><label>Latitude</label><input name=lat value=")HTML"+String(cfg.latitude,5)+R"HTML("></div></div><label>Longitude</label><input name=lon value=")HTML"+String(cfg.longitude,5)+R"HTML("><label class=check><input type=checkbox name=weather )HTML"+checked(cfg.showWeather)+R"HTML(> Show weather</label><label class=check><input type=checkbox name=hour24 )HTML"+checked(cfg.hour24)+R"HTML(> 24-hour clock</label></div>
  <div class=card><h3>TOUCH CALIBRATION</h3><div class=row><div><label>X min</label><input type=number name=txmin value=")HTML"+String(cfg.txMin)+R"HTML("></div><div><label>X max</label><input type=number name=txmax value=")HTML"+String(cfg.txMax)+R"HTML("></div><div><label>Y min</label><input type=number name=tymin value=")HTML"+String(cfg.tyMin)+R"HTML("></div><div><label>Y max</label><input type=number name=tymax value=")HTML"+String(cfg.tyMax)+R"HTML("></div></div><label class=check><input type=checkbox name=tswap )HTML"+checked(cfg.touchSwap)+R"HTML(> Swap XY</label><label class=check><input type=checkbox name=tinvx )HTML"+checked(cfg.touchInvX)+R"HTML(> Invert X</label><label class=check><input type=checkbox name=tinvy )HTML"+checked(cfg.touchInvY)+R"HTML(> Invert Y</label><div class=hint>Only change these if touch positions do not match the screen.</div></div>
  <button class=btn>Save & Restart</button> <a class="btn btn2" href=/sync>Sync now</a></form>
  <div class=card><h3>LOCAL FIRMWARE UPDATE</h3><form method=POST action=/update enctype=multipart/form-data><input type=file name=firmware accept=.bin required><br><br><button class="btn btn2">Upload firmware.bin</button></form></div>
  <div class=foot>Setup AP: SmartCalendar-Setup · http://192.168.4.1</div></div></body></html>)HTML";
  return h;
}

// ============================================================
// Cache
// ============================================================
String cleanField(String s){s.replace("\t"," ");s.replace("\r"," ");s.replace("\n"," ");return s;}
void saveEventCache(){
  File f=SPIFFS.open("/events.tsv","w"); if(!f)return;
  int n=min((int)events.size(),60); for(int i=0;i<n;i++){Event &e=events[i];f.printf("%lld\t%lld\t%d\t%s\t%s\t%s\n",(long long)e.start,(long long)e.end,e.allDay?1:0,cleanField(e.source).c_str(),cleanField(e.title).c_str(),cleanField(e.location).c_str());} f.close();
}
void loadEventCache(){
  File f=SPIFFS.open("/events.tsv","r"); if(!f)return; events.clear();
  while(f.available()){
    String line=f.readStringUntil('\n'); int p1=line.indexOf('\t'),p2=line.indexOf('\t',p1+1),p3=line.indexOf('\t',p2+1),p4=line.indexOf('\t',p3+1),p5=line.indexOf('\t',p4+1);
    if(p1<0||p2<0||p3<0||p4<0||p5<0)continue; Event e; e.start=(time_t)strtoll(line.substring(0,p1).c_str(),nullptr,10);e.end=(time_t)strtoll(line.substring(p1+1,p2).c_str(),nullptr,10);e.allDay=line.substring(p2+1,p3).toInt();e.source=line.substring(p3+1,p4);e.title=line.substring(p4+1,p5);e.location=line.substring(p5+1);e.location.trim();if(e.start>0)events.push_back(e);
  } f.close();
}

// ============================================================
// ICS parsing / sync
// ============================================================
String icsUnescape(String s){s.replace("\\n"," ");s.replace("\\N"," ");s.replace("\\,",",");s.replace("\\;",";");s.replace("\\\\","\\");s.trim();return s;}
time_t makeEpoch(int Y,int M,int D,int h,int m,int sec,bool isUtc){
  struct tm tmv={};tmv.tm_year=Y-1900;tmv.tm_mon=M-1;tmv.tm_mday=D;tmv.tm_hour=h;tmv.tm_min=m;tmv.tm_sec=sec;tmv.tm_isdst=-1;
  time_t t=mktime(&tmv); if(isUtc)t+=cfg.utcOffsetMinutes*60; return t;
}
time_t parseIcsTime(String v){
  v.trim();bool z=v.endsWith("Z");if(z)v.remove(v.length()-1);if(v.length()<8)return 0;
  int Y=v.substring(0,4).toInt(),M=v.substring(4,6).toInt(),D=v.substring(6,8).toInt(),h=0,m=0,s=0;
  if(v.length()>=15){h=v.substring(9,11).toInt();m=v.substring(11,13).toInt();s=v.substring(13,15).toInt();}
  return makeEpoch(Y,M,D,h,m,s,z);
}
String unfoldIcs(String body){body.replace("\r\n ","");body.replace("\r\n\t","");body.replace("\n ","");body.replace("\n\t","");return body;}
String getIcsValue(const String& block,const String& key){
  int pos=0;while(true){int i=block.indexOf(key,pos);if(i<0)return "";if(i==0||block[i-1]=='\n'||block[i-1]=='\r'){int colon=block.indexOf(':',i);if(colon<0)return "";int nl=block.indexOf('\n',colon);String v=block.substring(colon+1,nl<0?block.length():nl);v.trim();return icsUnescape(v);}pos=i+key.length();}
}
void parseIcs(const String& raw,const String& src,std::vector<Event>& out){
  String body=unfoldIcs(raw);int p=0;while(true){int b=body.indexOf("BEGIN:VEVENT",p);if(b<0)break;int e=body.indexOf("END:VEVENT",b);if(e<0)break;String block=body.substring(b,e);Event ev;ev.source=src;ev.title=getIcsValue(block,"SUMMARY");if(ev.title.length()==0)ev.title="Untitled event";ev.location=getIcsValue(block,"LOCATION");
    String sv=getIcsValue(block,"DTSTART");String evv=getIcsValue(block,"DTEND");ev.start=parseIcsTime(sv);ev.end=parseIcsTime(evv);ev.allDay=(sv.length()==8);if(ev.end==0)ev.end=ev.start+(ev.allDay?86400:3600);if(ev.start>0)out.push_back(ev);p=e+10;}
}
bool fetchIcs(String url,String src,std::vector<Event>& out){
  if(url.length()<8)return true;WiFiClientSecure client;client.setInsecure();HTTPClient http;http.setTimeout(15000);bool ok=false;
  if(http.begin(client,url)){http.setUserAgent("SmartCalendarPro/2.0");int code=http.GET();if(code==200){String body=http.getString();parseIcs(body,src,out);ok=true;}http.end();}return ok;
}
void syncCalendars(){
  if(WiFi.status()!=WL_CONNECTED){syncOk=false;syncMessage="Offline - cached events";return;}
  syncMessage="Syncing..."; std::vector<Event> fresh; bool g=fetchIcs(cfg.googleIcs,"GOOGLE",fresh); bool o=fetchIcs(cfg.outlookIcs,"OUTLOOK",fresh);
  if(g&&o){
    std::sort(fresh.begin(),fresh.end(),[](const Event&a,const Event&b){return a.start<b.start;}); events=fresh; saveEventCache(); syncOk=true; syncMessage="Synced"; lastSync=millis();
  }else{syncOk=false;syncMessage="Sync failed - cache kept";}
}

// ============================================================
// Weather (Open-Meteo, no API key)
// ============================================================
float jsonNumber(const String& s,const String& key,float fallback){int i=s.indexOf("\""+key+"\"");if(i<0)return fallback;i=s.indexOf(':',i);if(i<0)return fallback;int e=i+1;while(e<(int)s.length()&&(isDigit(s[e])||s[e]=='-'||s[e]=='+'||s[e]=='.'))e++;return s.substring(i+1,e).toFloat();}
void fetchWeather(){
  if(!cfg.showWeather||WiFi.status()!=WL_CONNECTED)return;String url="https://api.open-meteo.com/v1/forecast?latitude="+String(cfg.latitude,5)+"&longitude="+String(cfg.longitude,5)+"&current=temperature_2m,weather_code&timezone=auto";
  WiFiClientSecure client;client.setInsecure();HTTPClient http;http.setTimeout(10000);if(http.begin(client,url)){int code=http.GET();if(code==200){String b=http.getString();float t=jsonNumber(b,"temperature_2m",999);int c=(int)jsonNumber(b,"weather_code",-1);if(t<100&&c>=0){weather.temp=t;weather.code=c;weather.valid=true;weather.lastUpdate=millis();lastWeather=millis();}}http.end();}
}

// ============================================================
// Wi-Fi / Portal / OTA
// ============================================================
void applyForm(){
  cfg.name=server.arg("name");cfg.wifiSsid=server.arg("ssid");cfg.wifiPass=server.arg("pass");cfg.googleIcs=server.arg("gics");cfg.outlookIcs=server.arg("oics");
  cfg.utcOffsetMinutes=server.arg("utc").toInt();cfg.refreshMinutes=constrain(server.arg("refresh").toInt(),5,120);cfg.brightness=constrain(server.arg("bright").toInt(),5,100);cfg.latitude=server.arg("lat").toFloat();cfg.longitude=server.arg("lon").toFloat();
  cfg.showWeather=server.hasArg("weather");cfg.hour24=server.hasArg("hour24");cfg.txMin=server.arg("txmin").toInt();cfg.txMax=server.arg("txmax").toInt();cfg.tyMin=server.arg("tymin").toInt();cfg.tyMax=server.arg("tymax").toInt();cfg.touchSwap=server.hasArg("tswap");cfg.touchInvX=server.hasArg("tinvx");cfg.touchInvY=server.hasArg("tinvy");saveConfig();
}
void beginWebServer(){
  if(serverStarted)return;
  server.on("/",[](){server.send(200,"text/html",setupPage());});
  server.on("/save",HTTP_POST,[](){applyForm();server.send(200,"text/html","<html><body style='background:#0d1117;color:white;font-family:Arial;padding:40px'><h2>Saved</h2><p>Smart Calendar is restarting...</p></body></html>");delay(700);ESP.restart();});
  server.on("/sync",HTTP_GET,[](){syncCalendars();fetchWeather();server.sendHeader("Location","/",true);server.send(302,"text/plain","");});
  server.on("/update",HTTP_POST,[](){server.send(200,"text/plain",Update.hasError()?"UPDATE FAILED - device not restarted":"UPDATE OK - restarting");delay(800);if(!Update.hasError())ESP.restart();},[](){HTTPUpload& u=server.upload();if(u.status==UPLOAD_FILE_START){Update.begin(UPDATE_SIZE_UNKNOWN);}else if(u.status==UPLOAD_FILE_WRITE){Update.write(u.buf,u.currentSize);}else if(u.status==UPLOAD_FILE_END){Update.end(true);}});
  server.onNotFound([](){if(portalMode){server.sendHeader("Location","http://192.168.4.1/",true);server.send(302,"text/plain","");}else server.send(404,"text/plain","Not found");});
  server.begin();serverStarted=true;
}
void startPortal(){
  portalMode=true;WiFi.mode(WIFI_AP_STA);WiFi.softAP("SmartCalendar-Setup","12345678");dns.start(53,"*",WiFi.softAPIP());beginWebServer();
}
void stopPortal(){if(!portalMode)return;dns.stop();WiFi.softAPdisconnect(true);portalMode=false;}
bool connectWiFi(uint32_t timeoutMs=12000){
  if(cfg.wifiSsid.length()==0)return false;WiFi.mode(WIFI_STA);WiFi.setAutoReconnect(true);WiFi.persistent(false);WiFi.begin(cfg.wifiSsid.c_str(),cfg.wifiPass.c_str());unsigned long st=millis();while(WiFi.status()!=WL_CONNECTED&&millis()-st<timeoutMs)delay(200);if(WiFi.status()==WL_CONNECTED){beginWebServer();return true;}return false;
}
void syncClock(){
  if(WiFi.status()!=WL_CONNECTED)return;configTime(cfg.utcOffsetMinutes*60,0,"pool.ntp.org","time.google.com");unsigned long st=millis();while(time(nullptr)<1700000000&&millis()-st<8000)delay(150);timeValid=time(nullptr)>=1700000000;
}

// ============================================================
// UI helpers / icons
// ============================================================
void drawWifiIcon(int x,int y,bool on){uint16_t c=on?C_GREEN:C_MUTED;if(!on){drawLine(x,y,x+12,y+12,c);drawLine(x+12,y,x,y+12,c);return;}drawLine(x,y+4,x+3,y+1,c);drawLine(x+3,y+1,x+6,y,c);drawLine(x+6,y,x+9,y+1,c);drawLine(x+9,y+1,x+12,y+4,c);drawLine(x+3,y+7,x+6,y+5,c);drawLine(x+6,y+5,x+9,y+7,c);fillCircle(x+6,y+11,1,c);}
void drawCloudIcon(int x,int y,uint16_t c){fillCircle(x+9,y+7,6,c);fillCircle(x+16,y+5,8,c);fillCircle(x+24,y+9,6,c);fillRect(x+7,y+8,20,8,c);}
void drawSunIcon(int x,int y,uint16_t c){drawCircle(x+12,y+12,5,c);for(int a=0;a<360;a+=45){float r=a*PI/180;drawLine(x+12+cos(r)*8,y+12+sin(r)*8,x+12+cos(r)*11,y+12+sin(r)*11,c);}}
void drawWeatherIcon(int x,int y){if(!weather.valid){drawCloudIcon(x,y,C_MUTED);return;}if(weather.code<=1)drawSunIcon(x,y,C_YELLOW);else drawCloudIcon(x,y,C_MUTED);}
void sourceDot(int x,int y,const String& src){fillCircle(x,y,3,src=="GOOGLE"?C_GOOGLE:C_OUTLOOK);}
String shortTitle(String s,int n){s=asciiSafe(s);if((int)s.length()<=n)return s;return s.substring(0,max(0,n-2))+"..";}

void drawTopBar(){
  fillRect(0,0,240,34,C_BG);
  time_t now=time(nullptr);struct tm t;localtime_r(&now,&t);char b[40];
  if(timeValid){strftime(b,sizeof(b),cfg.hour24?"%H:%M":"%I:%M %p",&t);drawText(12,9,b,C_TEXT,C_BG,2);}
  else drawText(12,9,"--:--",C_TEXT,C_BG,2);
  drawWifiIcon(196,10,WiFi.status()==WL_CONNECTED);
  fillCircle(226,17,4,syncOk?C_GREEN:C_MUTED);
}
void drawBottomNav(){
  fillRect(0,282,240,38,C_SURFACE);drawHLine(0,282,240,C_BORDER);
  const int cx[3]={40,120,200};const char* labels[3]={"TODAY","MONTH","SET"};
  for(int i=0;i<3;i++){
    bool on=(int)activeScreen==i;uint16_t c=on?C_ACCENT:C_MUTED;
    if(on)fillRoundRect(cx[i]-32,288,64,24,12,C_SURF2);
    drawText(cx[i]-textW(labels[i],1)/2,297,labels[i],c,on?C_SURF2:C_SURFACE,1);
  }
}

bool sameDay(time_t a,time_t b){struct tm ta,tb;localtime_r(&a,&ta);localtime_r(&b,&tb);return ta.tm_year==tb.tm_year&&ta.tm_yday==tb.tm_yday;}
bool hasEventOnDate(int Y,int M,int D){for(auto &e:events){struct tm t;localtime_r(&e.start,&t);if(t.tm_year+1900==Y&&t.tm_mon+1==M&&t.tm_mday==D)return true;}return false;}

void drawHome(){
  fillScreen(C_BG);drawTopBar();time_t now=time(nullptr);struct tm t;localtime_r(&now,&t);char b[48];
  strftime(b,sizeof(b),"%A, %d %B",&t);drawText(12,42,b,C_MUTED,C_BG,1);

  if(cfg.showWeather){
    fillRoundRect(146,58,82,27,12,C_SURFACE);
    drawWeatherIcon(153,60);
    String wt=weather.valid?String((int)round(weather.temp))+" C":"-- C";
    drawTextRight(219,68,wt,C_TEXT,C_SURFACE,1);
  }

  fillRoundRect(10,92,220,78,14,C_SURFACE);drawText(20,102,"NEXT",C_ACCENT,C_SURFACE,1);
  int idx=-1;for(int i=0;i<(int)events.size();i++){if(events[i].end>=now-60){idx=i;break;}}
  if(idx>=0){
    Event &e=events[idx];struct tm et;localtime_r(&e.start,&et);char tmstr[12];
    strftime(tmstr,sizeof(tmstr),e.allDay?"ALL DAY":"%H:%M",&et);
    sourceDot(21,128,e.source);
    drawText(31,118,shortTitle(e.title,16),C_TEXT,C_SURFACE,2);
    drawText(31,145,String(tmstr)+"  "+e.source,C_MUTED,C_SURFACE,1);
  }else{
    drawText(20,126,"NO UPCOMING",C_MUTED,C_SURFACE,1);
    drawText(20,143,"EVENTS",C_MUTED,C_SURFACE,1);
  }

  drawText(12,184,"UP NEXT",C_MUTED,C_BG,1);
  int y=202,shown=0;
  for(auto &e:events){
    if(e.end<now-60)continue;
    if(idx>=0 && e.start==events[idx].start && e.title==events[idx].title)continue;
    struct tm et;localtime_r(&e.start,&et);char ts[8];strftime(ts,sizeof(ts),e.allDay?"DAY":"%H:%M",&et);
    sourceDot(17,y+7,e.source);drawText(27,y+2,ts,C_MUTED,C_BG,1);
    drawText(70,y+2,shortTitle(e.title,25),C_TEXT,C_BG,1);
    y+=24;if(++shown>=3)break;
  }
  if(shown==0)drawText(12,205,"YOUR DAY IS CLEAR",C_MUTED,C_BG,1);
  drawBottomNav();
}

int daysInMonth(int Y,int M){if(M==2){bool leap=(Y%400==0)||((Y%4==0)&&(Y%100!=0));return leap?29:28;}return (M==4||M==6||M==9||M==11)?30:31;}
int firstWeekdayMon0(int Y,int M){struct tm tt={};tt.tm_year=Y-1900;tt.tm_mon=M-1;tt.tm_mday=1;tt.tm_hour=12;mktime(&tt);return (tt.tm_wday+6)%7;}
void monthOffsetToYM(int off,int &Y,int &M){time_t now=time(nullptr);struct tm t;localtime_r(&now,&t);Y=t.tm_year+1900;M=t.tm_mon+1+off;while(M>12){M-=12;Y++;}while(M<1){M+=12;Y--;}}
void drawMonth(){
  fillScreen(C_BG);drawTopBar();int Y,M;monthOffsetToYM(selectedMonthOffset,Y,M);
  static const char* mons[]={"JANUARY","FEBRUARY","MARCH","APRIL","MAY","JUNE","JULY","AUGUST","SEPTEMBER","OCTOBER","NOVEMBER","DECEMBER"};
  fillRoundRect(10,42,220,34,12,C_SURFACE);
  drawText(22,54,"<",C_MUTED,C_SURFACE,1);
  String title=String(mons[M-1])+" "+Y;drawText(120-textW(title,1)/2,54,title,C_TEXT,C_SURFACE,1);
  drawText(214,54,">",C_MUTED,C_SURFACE,1);
  const char* wd[]={"M","T","W","T","F","S","S"};
  for(int c=0;c<7;c++)drawText(21+c*32,90,wd[c],c>=5?C_ACCENT:C_MUTED,C_BG,1);
  int first=firstWeekdayMon0(Y,M),dim=daysInMonth(Y,M);time_t now=time(nullptr);struct tm tn;localtime_r(&now,&tn);
  for(int d=1;d<=dim;d++){
    int cell=first+d-1,row=cell/7,col=cell%7;int cx=23+col*32,cy=116+row*27;
    bool today=(Y==tn.tm_year+1900&&M==tn.tm_mon+1&&d==tn.tm_mday);
    if(today){fillCircle(cx,cy+3,10,C_ACCENT);String ds=String(d);drawText(cx-textW(ds,1)/2,cy,ds,C_BLACK,C_ACCENT,1);}
    else{String ds=String(d);drawText(cx-textW(ds,1)/2,cy,ds,col>=5?C_MUTED:C_TEXT,C_BG,1);}
    if(hasEventOnDate(Y,M,d))fillCircle(cx,cy+16,2,today?C_WHITE:C_ACCENT);
  }
  drawBottomNav();
}

void drawSettings(){
  fillScreen(C_BG);drawTopBar();
  drawText(12,45,"DEVICE",C_MUTED,C_BG,1);
  fillRoundRect(10,59,220,54,12,C_SURFACE);
  drawText(20,70,shortTitle(cfg.name,22),C_TEXT,C_SURFACE,1);
  drawText(20,91,WiFi.status()==WL_CONNECTED?WiFi.localIP().toString():"OFFLINE",C_MUTED,C_SURFACE,1);
  drawTextRight(218,80,String(cfg.brightness)+"%",C_ACCENT,C_SURFACE,1);

  drawText(12,128,"SYNC",C_MUTED,C_BG,1);
  fillRoundRect(10,142,220,48,12,C_SURFACE);
  drawText(20,154,shortTitle(syncMessage,24),syncOk?C_GREEN:C_MUTED,C_SURFACE,1);
  String src=String(cfg.googleIcs.length()?"G":"-")+" / "+String(cfg.outlookIcs.length()?"O":"-");
  drawTextRight(218,173,src,C_TEXT,C_SURFACE,1);

  fillRoundRect(10,210,220,28,12,C_ACCENT);
  drawText(120-textW("SYNC NOW",1)/2,220,"SYNC NOW",C_BLACK,C_ACCENT,1);
  fillRoundRect(10,246,220,28,12,C_SURF2);
  drawText(120-textW("OPEN SETUP",1)/2,256,"OPEN SETUP",C_TEXT,C_SURF2,1);
  drawBottomNav();
}

void drawUI(){if(activeScreen==SCREEN_HOME)drawHome();else if(activeScreen==SCREEN_MONTH)drawMonth();else drawSettings();lastDraw=millis();}

// ============================================================
// Touch actions
// ============================================================
void handleTouch(){
  int x,y;if(!readTouch(x,y))return;if(millis()-lastTouch<250)return;lastTouch=millis();
  if(y>=282){
    if(x<80)activeScreen=SCREEN_HOME;else if(x<160)activeScreen=SCREEN_MONTH;else activeScreen=SCREEN_SETTINGS;
    drawUI();return;
  }
  if(activeScreen==SCREEN_MONTH&&y>=40&&y<=80){
    if(x<70){selectedMonthOffset--;drawUI();}
    else if(x>170){selectedMonthOffset++;drawUI();}
    return;
  }
  if(activeScreen==SCREEN_SETTINGS){
    if(y>=206&&y<=242){syncCalendars();fetchWeather();drawUI();return;}
    if(y>=242&&y<=280){startPortal();drawUI();return;}
  }
}

// ============================================================
// Setup / loop
// ============================================================
void setup(){
  Serial.begin(115200);pinMode(BOOT_BTN,INPUT_PULLUP);loadConfig();tftInit();touchInit();SPIFFS.begin(true);loadEventCache();
  fillScreen(C_BG);fillCircle(120,118,20,C_ACCENT);drawText(48,157,"SMART CALENDAR",C_TEXT,C_BG,2);drawText(108,184,"PRO",C_MUTED,C_BG,1);
  bool online=connectWiFi();if(online){syncClock();syncCalendars();fetchWeather();}else{syncMessage="Offline - cached events";startPortal();}
  drawUI();
}

void loop(){
  if(portalMode)dns.processNextRequest();if(serverStarted)server.handleClient();handleTouch();
  if(WiFi.status()==WL_CONNECTED){
    if(!timeValid)syncClock();
    if(millis()-lastSync>(unsigned long)cfg.refreshMinutes*60000UL){syncCalendars();drawUI();}
    if(cfg.showWeather&&millis()-lastWeather>30UL*60000UL){fetchWeather();if(activeScreen==SCREEN_HOME)drawUI();}
  }
  if(millis()-lastDraw>30000){drawUI();}
  static unsigned long bootPress=0;if(digitalRead(BOOT_BTN)==LOW){if(!bootPress)bootPress=millis();if(millis()-bootPress>3000&&!portalMode){startPortal();activeScreen=SCREEN_SETTINGS;drawUI();}}else bootPress=0;
  delay(8);
}
