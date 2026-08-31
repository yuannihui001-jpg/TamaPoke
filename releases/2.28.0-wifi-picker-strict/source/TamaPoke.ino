// TamaPoke - tamagotchi pixel art inspirado en la gen 1
// para Waveshare ESP32-S3-Touch-AMOLED-1.75
//
// Librerias (Library Manager o repo de Waveshare):
//   - "GFX Library for Arduino" (moononournation), con soporte CO5300 QSPI
//   - "SensorLib" (Lewis He), driver tactil CST9217
//
// Placa: ESP32S3 Dev Module | Flash 16MB | PSRAM: OPI PSRAM | USB CDC On Boot: Enabled
//
// Los sprites y la tabla de especies se generan con tools/sprites.py (emit).

#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include "Arduino_GFX_Library.h"
#include "TouchDrvCSTXXX.hpp"
#include "pin_config.h"
#include "species.h"
#include "dex.h"
#include "pet.h"
#include "sdmon.h"
#include "rtcbat.h"
#include "i18n.h"
#include "audio.h"
#include "cn_canvas.h"
#include "battle.h"
#include "shop.h"

// Version del firmware. Subir este numero en cada release (y manifest.json para
// el instalador web). Se muestra en la pantalla de ajustes y por serie al arrancar.
#define FW_VERSION "2.28.0"

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
  LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *panel = new Arduino_CO5300(
  bus, LCD_RESET, 0 /*rotation*/, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);
// Framebuffer completo en PSRAM: dibujamos todo y hacemos flush() (sin parpadeo)
CnCanvas *gfx = new CnCanvas(LCD_WIDTH, LCD_HEIGHT, panel);

// strlen() counts UTF-8 bytes, which would mis-center Chinese labels.
// Keep it as a code-point count for buffers and derive pixel widths separately.
static size_t uiTextLen(const char *s) {
  size_t n = 0;
  while (*s) {
    if ((((uint8_t)*s) & 0xC0) != 0x80) ++n;
    ++s;
  }
  return n;
}

// Return the rendered width for a string whose ASCII cell is asciiCell pixels.
// Chinese glyphs are 25 pixels wide instead of the ASCII font's 6-pixel cell;
// all centered labels use this helper so the larger font cannot overlap.
static int uiTextWidth(const char *s, int asciiCell) {
  if (gLang != LANG_ZH) return (int)uiTextLen(s) * asciiCell;
  // Chinese labels use a native 25x25 cell regardless of ASCII text size.
  const int cnScale = 1;
  int width = 0;
  while (*s) {
    uint8_t c = (uint8_t)*s++;
    if (c < 0x80) {
      width += asciiCell;
      continue;
    }
    width += CN_GLYPH_WIDTH * cnScale;
    if ((c & 0xE0) == 0xC0) s += 1;
    else if ((c & 0xF0) == 0xE0) s += 2;
    else if ((c & 0xF8) == 0xF0) s += 3;
  }
  return width;
}
#define strlen uiTextLen

TouchDrvCST92xx touch;
Pet pet;
Preferences uiPrefs;
String authToken;

// sprite animado de la SD para la especie actual (si existe el archivo)
SdMon mon;          // sprite B/N (respaldo y minijuego si no hay PMD)
PmdMon pmd;         // sprite PMD multi-accion (pantalla principal)
PmdMon evoPmd;      // forma anterior, solo durante el parpadeo de evolucion
PmdMon wildPmd;     // rival salvaje de la batalla; usa la misma SD de sprites
int16_t monFor = -2;
bool monShinyFor = false;

// comportamiento del bicho en pantalla
struct {
  uint8_t mode = 0;     // 0 idle, 1 paseo, 2 gesto one-shot
  uint8_t act = PMD_IDLE;
  uint32_t t0 = 0;      // inicio de la animacion en curso
  uint32_t until = 0;   // fin del estado actual
  float x = 233, targetX = 233;
} beh;
#define PET_GROUND 304  // linea de suelo de la mascota
PmdMon galleryPmd;  // sprite grande de la vista detalle de la galeria (PMD/TPK2, legal)

// galeria pokedex
bool galleryOpen = false;
bool galleryDirty = false;
int galleryPage = 0;        // 10 paginas de 16
int16_t galleryDetail = 0;  // dex en vista detalle, 0 = rejilla

bool screenOff = false;       // pulsacion corta del boton PWR
bool cardOpen = false;        // ficha del bicho (deslizar vertical)
bool kbOpen = false;          // teclado para renombrar al bicho
char nameBuf[64] = "";
uint8_t nameLen = 0;
uint8_t kbMode = 0;           // 0 = renombrar, 1 = contraseña Wi-Fi
uint8_t wifiKbPage = 0;
bool wifiPickerOpen = false;
uint8_t wifiNetworkCount = 0;
char wifiSsids[6][33] = {};
int8_t wifiSelected = -1;
uint8_t cardPage = 0;         // 0 perfil, 1 stats+medallas
bool clockOpen = false;       // pantalla de ajuste de hora (deslizar abajo)
int clockH = 12, clockM = 0;  // hora en edicion

// escena de bano: espuma sobre el bicho y limpieza al reventar
uint32_t bathUntil = 0;
bool bathPending = false;
struct { int16_t x, y; uint8_t r, ph; } bubbles[14];
uint32_t feedMenuUntil = 0;   // selector de comida abierto hasta este millis
uint8_t feedSlots[4] = { 0, 0, 0, 0 };
uint8_t feedCount = 0;
uint32_t toyMenuUntil = 0;    // selector de juguetes comprados
uint8_t toySlots[4] = { 0, 0, 0, 0 };
uint8_t toyCount = 0;
uint32_t poopCleanMsgUntil = 0;
uint32_t sleepTouchUntil = 0;
uint8_t brightnessSetting = 180;
uint8_t volumeLevel = 1;
uint8_t touchSensitivity = 55; // gesture threshold in pixels; lower is more sensitive
bool wifiConnecting = false;
uint32_t wifiConnectStarted = 0;
uint32_t wifiNoticeUntil = 0;
char wifiNotice[48] = "";
// 将最新 app 分区镜像放到这个公开地址后，设置页的“更新”即可 OTA。
static const char *const AUTH_SERVER_URL = "https://tamapoke-license.yuannihui001.workers.dev";

// minijuego "toques": mantener la pokeball en el aire
bool gameOpen = false;
uint32_t gameOverUntil = 0;
float ballX, ballY, ballVX, ballVY, gamePetX;
uint8_t gameScore, gameMisses;
float hitX, hitY;             // ultimo golpe (anillo de impacto)
uint32_t hitTime = 0;
bool gameNewHi = false;

// Menu y juego de memoria 4x4 portados de TamaPetchi. Se mantiene separado
// del juego de la pokeball para no cambiar su fisica ni sus records.
bool gameMenuOpen = false;
bool memoryOpen = false;
enum MemoryPhase : uint8_t { MEM_SHOWING, MEM_INPUT, MEM_RESULT };
MemoryPhase memoryPhase = MEM_SHOWING;
uint8_t memorySequence[30] = { 0 };
uint8_t memoryRound = 1;       // longitud actual de la secuencia (1..30)
uint8_t memoryStep = 0;        // paso que se esta mostrando
uint8_t memoryInput = 0;       // entrada que esperamos del jugador
uint16_t memoryScore = 0;      // aciertos acumulados en toda la partida
int8_t memoryFlash = -1;
uint32_t memoryNextMs = 0;
uint32_t memoryFlashUntil = 0;
uint32_t memoryResultUntil = 0;
bool memoryWon = false;
uint32_t memorySession = 0;   // invalidates the memory-game frame cache on restart

// Mundo persistente: la casa conserva el bioma del Pokemon; las otras salas
// son destinos visuales con un coste pequeno y un bonus de felicidad.
bool worldOpen = false;
bool shopOpen = false;
bool decorOpen = false;
bool warehouseOpen = false;
uint8_t shopPage = 0;        // 0 食品，1 用品
int8_t shopCategory = -1;    // -1 分类首页，0..5 商品列表
uint8_t shopScroll = 0;
bool shopFromGallery = false;  // tienda abierta con el gesto lateral de la Pokedex
uint32_t economyMsgUntil = 0;
bool economyMsgOk = false;
bool shopDetailOpen = false;
uint8_t shopDetailSlot = 0;
uint8_t shopDetailQty = 1;
uint32_t shopSession = 0;      // invalidates the shop frame cache on each open/close
bool travelOpen = false;
uint8_t travelSlot = 0;
uint32_t travelUntil = 0;
uint8_t warehouseScroll = 0;
#define WORLD_HOME 0
#define WORLD_PARK 1
#define WORLD_BEACH 2
#define WORLD_FOREST 3

#define MEM_GRID_X 104
#define MEM_GRID_Y 104
#define MEM_CELL 58
#define MEM_GAP 8
#define MEM_GRID_STEP (MEM_CELL + MEM_GAP)

// saco de entrenamiento (entrena la fuerza)
bool sackOpen = false;
uint32_t sackUntil = 0, sackOverUntil = 0;
uint16_t sackHits = 0;
float sackShake = 0;
uint8_t sackGain = 0;
bool sackNewHi = false;

// batalla salvaje manual: se abre desde el menu JUEGOS y no interrumpe la
// crianza por sorpresa. Tras una victoria hay una sola oportunidad de captura.
bool battleOpen = false;
bool battleResolved = false;
bool battleCatchOffered = false;
bool battleCatchDone = false;
bool battleCatchTried = false;
bool battleCatchSuccess = false;
bool battleDirty = true;       // 战斗画面只有状态变化时才重绘，避免触摸闪屏
uint32_t battleSession = 0;
int16_t battleDex = 1;
uint8_t battleLevel = 1;
BattleStats battlePlayer = {};
BattleStats battleEnemy = {};
BattleRuntime battleRun = {};
BattleTurnResult battleTurn = {};
char battleMsg[40] = "";

// las 9 especies con sprite propio en flash (respaldo sin SD): dex -> indice
int flashIdxForDex(int16_t dex) {
  static const int8_t IDX[10] = { -1, 3, 4, 5, 0, 1, 2, 6, 7, 8 };
  return (dex >= 1 && dex <= 9) ? IDX[dex] : -1;
}

#define CX 233  // centro de la pantalla redonda
#define CY 233
#define PET_CY 202  // centro vertical del sprite

static const uint16_t INK_K = 0x18C4;  // spriteColor('k')

// botones de icono siguiendo el arco inferior de la pantalla redonda
// (los exteriores van mas altos para no salirse del circulo)
struct Btn {
  int16_t cx, cy;
  const char *const *icon;
};
Btn buttons[4] = {
  { 140, 398, SPR_ICON_FOOD },   // comer
  { 202, 412, SPR_ICON_PLAY },   // jugar
  { 264, 412, SPR_ICON_LIGHT },  // luz
  { 326, 398, SPR_ICON_CLEAN },  // bano
};
#define BTN_HALF 26  // boton de 52x52
#define BTN_HIT 36   // radio tactil (un poco mas generoso)

// grietas del huevo (pixeles 'k' sobre el sprite)
static const uint8_t CRACK1[][2] = { {15,8},{16,9},{15,10} };
static const uint8_t CRACK2[][2] = { {11,13},{12,14},{11,15},{20,12},{19,13},{20,14} };
// estrellas del modo noche
static const uint16_t STARS[][2] = { {120,140},{330,120},{370,210},{95,230},{280,90},{160,95} };

bool wasPressed = false;
// eleccion de inicial (primera partida): Bulbasaur / Charmander / Squirtle, 3 filas
static const int16_t STARTER_DEX[3] = { 1, 4, 7 };
#define STARTER_ROW_Y 110
#define STARTER_ROW_H 70
#define STARTER_ROW_GAP 8
// boton-CTA de evolucion (centrado, mitad de pantalla)
#define EVO_BTN_W 256
#define EVO_BTN_H 64
#define EVO_BTN_X (CX - EVO_BTN_W / 2)
#define EVO_BTN_Y 172
// boton-CTA de despedida (mas ancho: lleva el nombre + frase)
#define FAR_BTN_W 408
#define FAR_BTN_H 58
#define FAR_BTN_X (CX - FAR_BTN_W / 2)
#define FAR_BTN_Y 176
// el CST9217 avisa por el pin INT cuando hay datos tactiles; lo usamos para no
// leer el bus I2C mientras el chip esta dormido (esa lectura se colgaba ~1s)
volatile bool gTouchIrq = false;
void IRAM_ATTR touchIsr() { gTouchIrq = true; }
uint32_t lastRender = 0;
void openGameMenu();
void gameMenuTap(int16_t x, int16_t y);
void startSack();
void startMemoryGame();
void memoryTap(int16_t x, int16_t y);
void renderGameMenu();
static void drawTopCoins(int right, int y, uint16_t color);
void renderMemoryGame();
void openWorldMenu();
void worldTap(int16_t x, int16_t y);
void shopTap(int16_t x, int16_t y);
static void closeShop();
void decorTap(int16_t x, int16_t y);
void renderWorldMenu();
void renderShop();
void renderTravel();
void renderDecor();
void renderWarehouse();
void warehouseTap(int16_t x, int16_t y);
void serviceWifi();
void startWifiConnection();
void openWifiPicker();
void wifiPickerTap(int16_t x, int16_t y);
void openWifiPassword(int8_t index);
void connectSelectedWifi();
void onlineUpdate();
void activateLicense(const String &license);
String deviceIdString();
void drawWorldBadge();
void startBattle();
void battleTap(int16_t x, int16_t y);
void renderBattle();
void drawPmdActM(PmdMon &m, uint8_t actId, int cx, int groundY, uint32_t t, bool loop, bool sil, uint8_t maxS);
static void drawProductIcon(uint8_t category, uint8_t slot, int cx, int cy, uint8_t s);
// proteccion del AMOLED: atenuado por inactividad
uint32_t lastInteract = 0;
uint8_t dimStage = 0;        // 0 despierto, 1 atenuado (90s), 2 casi apagado (5min)
bool swallowGesture = false; // el toque que despierta no acciona nada
uint32_t holdStart = 0;     // pulsacion larga sobre el bicho
uint32_t confirmUntil = 0;  // dialogo "soltar?" activo hasta este millis
uint8_t choiceKind = 0;     // dialogo de decision: 0 ninguno, 1 evolucion, 2 despedida
uint32_t choiceUntil = 0;   // se cierra solo a este millis
int16_t tX0, tY0, tXl, tYl; // gesto en curso (inicio y ultima posicion)
uint32_t tStart = 0;
bool holdFired = false;

void setup() {
  Serial.setRxBufferSize(8192);  // la transferencia a SD llega en bloques de 2 KB
  Serial.begin(115200);
  // CRITICO: sin esto, Serial.print BLOQUEA el juego cuando no hay un
  // monitor serie abierto en el host (el bufer TX del USB CDC se llena
  // y nadie lo vacia) -> con timeout 0 los mensajes se descartan
  Serial.setTxTimeoutMs(0);
  Serial.printf("TamaPoke fw v%s\n", FW_VERSION);
  loadLang();  // idioma guardado (ZH por defecto)
  uiPrefs.begin("ui", false);
  brightnessSetting = constrain(uiPrefs.getUChar("bright", 180), 20, 255);
  volumeLevel = min((uint8_t)2, uiPrefs.getUChar("volume", 1));
  touchSensitivity = constrain(uiPrefs.getUChar("touch", 55), 32, 80);
  authToken = uiPrefs.getString("auth", "");
  Wire.begin(IIC_SDA, IIC_SCL);
  // CST9217 (tactil), AXP2101 (PMU) y PCF85063 (RTC) comparten este bus I2C.
  // Red de seguridad para PMU/RTC (SensorLib NO respeta este timeout en el
  // tactil; el cuelgue del tactil dormido se resuelve gateando por INT, ver
  // handleTouch).
  Wire.setTimeOut(50);

  // CRITICO: encender la alimentacion del panel (BLDO1=OLED VDD 3.3V) ANTES de
  // inicializar el display. Si el PMU se reseteo (drenaje total), este rail
  // queda OFF y la pantalla se ve negra aunque el resto de la placa funcione.
  pmuEnablePanel();

  // QSPI a 80MHz (por defecto 40): el flush del framebuffer es el cuello de
  // botella del fps (~56ms a 40MHz). Si el panel mostrara basura, bajar a 40M.
  if (!gfx->begin(80000000)) Serial.println("gfx->begin() fallo");
  panel->setBrightness(brightnessSetting);

  touch.setPins(TP_RESET, TP_INT);
  bool touchOk = false;
  for (int i = 0; i < 3 && !touchOk; i++) {  // a veces falla al primer intento
    touchOk = touch.begin(Wire, 0x5A, IIC_SDA, IIC_SCL);
    if (!touchOk) delay(150);
  }
  if (!touchOk) Serial.println("CST9217 no detectado");
  // begin() deja el chip en modo comando (lee la identidad y no sale);
  // hace falta un reset por hardware para que vuelva a reportar toques
  touch.reset();
  touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
  touch.setMirrorXY(true, true);  // el panel esta montado girado 180 grados
  // INT activo-bajo: salta cuando hay datos. Gatea las lecturas I2C (ver loop)
  pinMode(TP_INT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(TP_INT), touchIsr, FALLING);

  pet.begin();
  sdBegin();
  thumbs.load();

  // reloj real: aplica el tiempo que estuvo apagado
  rtcBegin();
  batBegin();
  pwrSetup();
  uint32_t e = rtcEpoch();
  if (e == 0) {
    rtcSetEpoch(1767225600UL);  // RTC virgen: semilla (la hora absoluta da igual,
    e = rtcEpoch();             // solo importan las diferencias)
    Serial.println("RTC sin hora: sembrado, sin progresion offline esta vez");
  }
  pet.syncClock(e);

  audioBegin();  // ES8311 + I2S + amplificador (suena un jingle de arranque)

  lastInteract = millis();
}

// carga/descarga el sprite de SD cuando cambia la especie
void ensureMon() {
  if (pet.speciesId == monFor && monShinyFor == pet.shiny && !sdDirty) return;
  sdDirty = false;
  monFor = pet.speciesId;
  monShinyFor = pet.shiny;
  mon.unload();
  pmd.unload();
  beh.x = beh.targetX = 233;
  beh.mode = 0;
  beh.until = 0;
  if (pet.speciesId >= 1 && pet.speciesId <= DEX_COUNT) {
    pmd.load(pet.speciesId, pet.shiny);          // principal: PMD
    if (!pmd.loaded) mon.load(pet.speciesId, pet.shiny);  // respaldo: B/N
  }
}

void loop() {
  uint32_t now = millis();
  pet.update(now);

  // avisa con un sonido cuando el bicho pasa a estar listo para evolucionar
  // (incluye el caso de cumplir al despertar). canEvolveNow es false durmiendo.
  static bool wasEvoReady = false;
  bool evoReady = pet.wantEvolveButton();
  if (evoReady && !wasEvoReady) sfxPlay(SFX_MEDAL);
  wasEvoReady = evoReady;
  // aviso sombrio cuando el bicho esta a punto de escaparse por abandono
  static bool wasRunReady = false;
  bool runReady = pet.canRunawayNow();
  if (runReady && !wasRunReady) sfxPlay(SFX_DENY);
  wasRunReady = runReady;

  handleTouch();
  handleSerial();
  serviceWifi();
  ensureMon();

  // 短按 PWR 切换屏幕灯光；长按由 AXP2101 硬件负责关机/开机。
  static uint32_t lastPwr = 0;
  if (now - lastPwr > 250) {
    lastPwr = now;
    if (pwrShortPressed()) {
      screenOff = !screenOff;
      if (!screenOff) lastInteract = now;
    }
  }

  updateBrightness(now);

  // vuelca el autoguardado periodico SOLO con la pantalla atenuada/apagada o
  // durmiendo: la escritura a NVS congela ~1s ambos cores (caché de flash off),
  // y aqui no hay animacion que se corte ni dedo esperando respuesta. Con 90s
  // de inactividad la pantalla ya atenua, asi que se vuelca enseguida; el uso
  // activo persiste igual por los guardados de cada accion (comer/jugar/...).
  if (pet.savePending() && (screenOff || dimStage >= 1 || pet.sleeping)) {
    pet.flushSave();
  }

  // anota la hora real cada 30 s (se persiste en cada save del juego)
  static uint32_t lastClock = 0;
  if (now - lastClock > 30000) {
    lastClock = now;
    uint32_t e = rtcEpoch();
    if (e) pet.lastSeenEpoch = e;
  }

  // latido de salud cada 5 min (para el soak test; se descarta si no hay monitor)
  static uint32_t lastHealth = 0;
  if (now - lastHealth > 300000) {
    lastHealth = now;
    Serial.printf("HEALTH up=%lus heap=%u min=%u\n", (unsigned long)(now / 1000),
                  ESP.getFreeHeap(), ESP.getMinFreeHeap());
  }

  // 85 ms en juego/saco: margen seguro para que el redibujado no pise el envio
  // DMA del frame anterior (a 40-65 ms solapaba y causaba flashes negros; con
  // sprites grandes el dibujo tarda mas, asi que se deja colchon)
  if (now - lastRender >= (uint32_t)((gameOpen || memoryOpen || gameMenuOpen || worldOpen || shopOpen || warehouseOpen || decorOpen || sackOpen || battleOpen) ? 85 : 100)) {
    lastRender = now;
    render();
  }
}

// brillo segun sueno + inactividad (proteccion del AMOLED)
void updateBrightness(uint32_t now) {
  // los eventos visibles despiertan la pantalla solos
  if (pet.evolving() || pet.ceremony || pet.eating() || pet.showHeart()) {
    lastInteract = now;
  }
  uint32_t idle = now - lastInteract;
  dimStage = (idle > 300000) ? 2 : (idle > 90000) ? 1 : 0;
  uint8_t target = pet.sleeping ? 0 : (usbPresent() ? brightnessSetting : (brightnessSetting > 35 ? brightnessSetting - 35 : 20));
  if (pet.sleeping && sleepTouchUntil > now) target = 18;  // 触摸只微亮，不唤醒
  if (dimStage == 1 && !pet.sleeping) target = brightnessSetting / 3;
  else if (dimStage == 2 && !pet.sleeping) target = 8;
  if (screenOff) target = 0;
  static uint8_t current = 255;
  if (target != current) {
    current = target;
    panel->setBrightness(target);
  }
}

static void setWifiNotice(const char *msg, uint32_t ms = 5000) {
  strncpy(wifiNotice, msg ? msg : "", sizeof(wifiNotice) - 1);
  wifiNotice[sizeof(wifiNotice) - 1] = 0;
  wifiNoticeUntil = millis() + ms;
}

String deviceIdString() {
  uint64_t chip = ESP.getEfuseMac();
  char id[17];
  snprintf(id, sizeof(id), "%08lX%08lX", (unsigned long)(chip >> 32), (unsigned long)chip);
  return String(id);
}

void startWifiConnection() {
  String ssid = uiPrefs.getString("ssid", "");
  String pass = uiPrefs.getString("pass", "");
  if (!ssid.length()) {
    setWifiNotice("请串口设置 WIFI 名称和密码");
    Serial.println("请发送: WIFI <SSID> <PASSWORD>");
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false);
  WiFi.begin(ssid.c_str(), pass.c_str());
  wifiConnecting = true;
  wifiConnectStarted = millis();
  setWifiNotice("正在连接 WiFi...", 12000);
}

// 扫描附近网络并显示一个居中的选择面板。扫描结果只保留前 6 个，避免
// 圆形屏幕上下边缘被列表裁切；密码输入沿用同一套触摸键盘。
void openWifiPicker() {
  wifiPickerOpen = true;
  wifiSelected = -1;
  wifiNetworkCount = 0;
  WiFi.mode(WIFI_STA);
  int n = WiFi.scanNetworks(false, true);
  if (n > 0) {
    wifiNetworkCount = (uint8_t)min(n, 6);
    for (uint8_t i = 0; i < wifiNetworkCount; ++i) {
      String ssid = WiFi.SSID(i);
      if (!ssid.length()) ssid = "(隐藏网络)";
      strncpy(wifiSsids[i], ssid.c_str(), sizeof(wifiSsids[i]) - 1);
      wifiSsids[i][sizeof(wifiSsids[i]) - 1] = 0;
    }
  }
  WiFi.scanDelete();
}

void openWifiPassword(int8_t index) {
  if (index < 0 || index >= wifiNetworkCount) return;
  wifiSelected = index;
  wifiPickerOpen = false;
  kbOpen = true;
  kbMode = 1;
  wifiKbPage = 0;
  nameLen = 0;
  nameBuf[0] = 0;
}

void connectSelectedWifi() {
  if (wifiSelected < 0 || wifiSelected >= wifiNetworkCount) return;
  uiPrefs.putString("ssid", wifiSsids[wifiSelected]);
  uiPrefs.putString("pass", nameBuf);
  kbOpen = false;
  kbMode = 0;
  startWifiConnection();
  clockOpen = true;
}

void wifiPickerTap(int16_t x, int16_t y) {
  if (y >= 92 && y < 92 + (int)wifiNetworkCount * 42 && x >= 58 && x <= 408) {
    int index = (y - 92) / 42;
    if (index >= 0 && index < wifiNetworkCount) openWifiPassword((int8_t)index);
    return;
  }
  if (y >= 370 && y <= 428) {
    if (x >= 70 && x < 210) { openWifiPicker(); return; }
    if (x >= 256 && x < 396) { wifiPickerOpen = false; return; }
  }
}

void renderWifiPicker() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  const char *title = "选择 WiFi";
  gfx->setCursor(CX - uiTextWidth(title, 6) / 2, 38);
  gfx->print(title);
  if (!wifiNetworkCount) {
    gfx->setTextSize(1);
    const char *empty = "未发现 WiFi，请重新扫描";
    gfx->setCursor(CX - uiTextWidth(empty, 6) / 2, 180);
    gfx->print(empty);
  }
  for (uint8_t i = 0; i < wifiNetworkCount; ++i) {
    int y = 92 + i * 42;
    gfx->fillRoundRect(58, y, 350, 34, 8, UI_WHITE);
    gfx->drawRoundRect(58, y, 350, 34, 8, UI_INK);
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(1);
    String ssid = wifiSsids[i];
    if (ssid.length() > 12) ssid = ssid.substring(0, 12);
    gfx->setCursor(CX - uiTextWidth(ssid.c_str(), 6) / 2, y + 8);
    gfx->print(ssid.c_str());
  }
  gfx->fillRoundRect(70, 382, 140, 40, 9, UI_WHITE);
  gfx->drawRoundRect(70, 382, 140, 40, 9, UI_INK);
  gfx->fillRoundRect(256, 382, 140, 40, 9, UI_WHITE);
  gfx->drawRoundRect(256, 382, 140, 40, 9, UI_INK);
  gfx->setTextColor(UI_INK); gfx->setTextSize(1);
  gfx->setCursor(70 + (140 - uiTextWidth("重新扫描", 6)) / 2, 396); gfx->print("重新扫描");
  gfx->setCursor(256 + (140 - uiTextWidth("返回", 6)) / 2, 396); gfx->print("返回");
  gfx->flush();
}

void serviceWifi() {
  if (!wifiConnecting) return;
  wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    wifiConnecting = false;
    String msg = String("WiFi 已连接 ") + WiFi.localIP().toString();
    setWifiNotice(msg.c_str(), 8000);
    return;
  }
  if (millis() - wifiConnectStarted > 12000) {
    wifiConnecting = false;
    WiFi.disconnect(false);
    setWifiNotice("WiFi 连接超时");
  }
}

void activateLicense(const String &license) {
  if (WiFi.status() != WL_CONNECTED) {
    setWifiNotice("请先连接 WiFi");
    return;
  }
  if (!license.length()) {
    setWifiNotice("许可码不能为空");
    return;
  }
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = String(AUTH_SERVER_URL) + "/v1/activate";
  if (!http.begin(client, url)) {
    setWifiNotice("授权服务无法打开");
    return;
  }
  http.addHeader("Content-Type", "application/json");
  String body = String("{\"license\":\"") + license + "\",\"deviceId\":\"" + deviceIdString() + "\"}";
  int code = http.POST(body);
  String response = code > 0 ? http.getString() : "";
  http.end();
  int start = response.indexOf("\"token\":\"");
  if (code != HTTP_CODE_OK || start < 0) {
    Serial.printf("授权失败 HTTP %d\n", code);
    setWifiNotice(code == 409 ? "许可已绑定其他设备" : "许可码无效");
    return;
  }
  start += 9;
  int end = response.indexOf('"', start);
  if (end <= start) {
    setWifiNotice("授权响应无效");
    return;
  }
  authToken = response.substring(start, end);
  uiPrefs.putString("auth", authToken);
  Serial.printf("设备已授权 ID=%s\n", deviceIdString().c_str());
  setWifiNotice("授权成功");
}

void onlineUpdate() {
  if (WiFi.status() != WL_CONNECTED) {
    setWifiNotice("请先连接 WiFi");
    return;
  }
  if (!authToken.length()) {
    setWifiNotice("请先输入作者许可码");
    Serial.printf("设备 ID: %s\n", deviceIdString().c_str());
    Serial.println("请发送: LICENSE <许可码>");
    return;
  }
  setWifiNotice("正在下载更新，请勿断电", 30000);
  Serial.printf("OTA %s\n", AUTH_SERVER_URL);
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  String url = String(AUTH_SERVER_URL) + "/v1/firmware";
  if (!http.begin(client, url)) {
    setWifiNotice("更新地址无法打开");
    return;
  }
  http.addHeader("Authorization", String("Bearer ") + authToken);
  http.addHeader("X-TamaPoke-Device", deviceIdString());
  http.addHeader("X-TamaPoke-Version", FW_VERSION);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("OTA HTTP %d\n", code);
    http.end();
    setWifiNotice("在线更新失败");
    return;
  }
  int total = http.getSize();
  if (total <= 0 || !Update.begin((size_t)total)) {
    http.end();
    setWifiNotice("更新空间不足");
    return;
  }
  WiFiClient *stream = http.getStreamPtr();
  size_t written = Update.writeStream(*stream);
  bool ok = written == (size_t)total && Update.end(true) && Update.isFinished();
  http.end();
  if (!ok) {
    Serial.printf("OTA 写入失败 %u/%u\n", (unsigned)written, (unsigned)total);
    setWifiNotice("在线更新失败");
    return;
  }
  setWifiNotice("更新完成，正在重启", 2000);
  delay(500);
  ESP.restart();
}

// ---------- consola serie (provision de SD + depuracion) ----------

void handleSerial() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;
  if (line == "DEVICEID") {
    Serial.printf("device=%s\n", deviceIdString().c_str());
    Serial.println("DONE");
    return;
  }
  if (line.startsWith("LICENSE ")) {
    activateLicense(line.substring(8));
    return;
  }
  if (line == "AUTHSTATUS") {
    Serial.printf("authorized=%d device=%s\n", authToken.length() > 0, deviceIdString().c_str());
    Serial.println("DONE");
    return;
  }
  if (line == "AUTHCLEAR") {
    authToken = "";
    uiPrefs.remove("auth");
    Serial.println("授权已清除");
    return;
  }
  if (line.startsWith("WIFI ")) {
    String args = line.substring(5);
    int split = args.indexOf(' ');
    if (split <= 0) {
      Serial.println("用法: WIFI <SSID> <PASSWORD>");
      return;
    }
    String ssid = args.substring(0, split);
    String pass = args.substring(split + 1);
    uiPrefs.putString("ssid", ssid);
    uiPrefs.putString("pass", pass);
    Serial.printf("WiFi 凭据已保存: %s\n", ssid.c_str());
    startWifiConnection();
    return;
  }
  if (line == "WIFIOFF") {
    wifiConnecting = false;
    WiFi.disconnect(true);
    setWifiNotice("WiFi 已关闭");
    Serial.println("DONE");
    return;
  }
  if (line == "WIFISTATUS") {
    Serial.printf("wifi=%d ssid=%s ip=%s\n", WiFi.status() == WL_CONNECTED,
                  WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    Serial.println("DONE");
    return;
  }
  if (sdSerialCommand(line)) return;

  if (line == "HATCH") {
    pet.eggTap(); pet.eggTap(); pet.eggTap();
    Serial.println("DONE");
  } else if (line.startsWith("SPEC ")) {
    int n = line.substring(5).toInt();
    if (n >= 1 && n <= DEX_COUNT) {
      pet.prevSpeciesId = pet.speciesId;
      pet.speciesId = n;
      Serial.printf("especie #%d %s\n", n, DEX_TBL[n].name);
    }
    Serial.println("DONE");
  } else if (line.startsWith("LVL ")) {
    pet.ageMinutes = (uint32_t)line.substring(4).toInt() * MINUTES_PER_LEVEL;
    Serial.println("DONE");
  } else if (line.startsWith("TIME ")) {
    uint32_t e = (uint32_t)line.substring(5).toInt();
    rtcSetEpoch(e);
    pet.setClock(e);
    Serial.printf("rtc=%u\n", rtcEpoch());
    Serial.println("DONE");
  } else if (line.startsWith("RTCSET ")) {  // solo RTC (simular apagados en pruebas)
    rtcSetEpoch((uint32_t)line.substring(7).toInt());
    Serial.printf("rtc=%u\n", rtcEpoch());
    Serial.println("DONE");
  } else if (line == "TIME") {
    Serial.printf("rtc=%u\n", rtcEpoch());
    Serial.println("DONE");
  } else if (line == "GAL") {
    galleryOpen = !galleryOpen;
    galleryDetail = 0;
    galleryDirty = true;
    if (!galleryOpen) galleryPmd.unload();
    Serial.println("DONE");
  } else if (line == "EGGS") {
    // simula 20 tiradas de huevo (no cambia el estado del juego)
    for (int i = 0; i < 20; i++) {
      int16_t d = pet.pickEggSpecies();
      Serial.printf("%d:%s(r%u) ", d, DEX_TBL[d].name, DEX_TBL[d].rarity);
    }
    Serial.println();
    Serial.println("DONE");
  } else if (line == "SHINY") {  // alterna shiny del actual (pruebas)
    pet.shiny = !pet.shiny;
    Serial.printf("shiny=%d\n", pet.shiny);
    Serial.println("DONE");
  } else if (line.startsWith("NICK ")) {
    pet.rename(line.substring(5).c_str());
    Serial.printf("nick=%s\n", pet.nick);
    Serial.println("DONE");
  } else if (line == "CAREDAY") {  // simula un dia nuevo cuidado (pruebas)
    pet.setClock(pet.lastSeenEpoch + 86400);
    pet.caress();
    Serial.printf("streak=%u bond=%u medals=0x%X\n", pet.streak, pet.bond, pet.medals);
    Serial.println("DONE");
  } else if (line == "BYE") {
    pet.startFarewell();
    Serial.println("DONE");
  } else if (line == "RUN") {
    pet.startRunaway();
    Serial.println("DONE");
  } else if (line == "BEEP") {
    sfxPlay(SFX_HATCH);  // prueba de audio
    Serial.println("DONE");
  } else if (line == "ABANDON") {
    pet.dbgRunawayReady();  // fuerza el estado "lista para escaparse" (test del boton)
    Serial.println("DONE");
  } else if (line == "WIPE") {
    pet.factoryReset();     // borra NVS y reinicia -> partida nueva (eleccion de inicial)
    Serial.println("DONE");
    delay(100);
    ESP.restart();
  } else if (line == "REG") {
    Serial.printf("pokedex %u/151:", pet.registeredCount());
    for (int i = 1; i <= 151; i++)
      if (pet.isRegistered(i)) Serial.printf(" %d", i);
    Serial.println();
    Serial.println("DONE");
  } else if (line == "HEALTH") {
    Serial.printf("up=%lus heap=%u min=%u sd=%d mon=%d\n",
                  (unsigned long)(millis() / 1000), ESP.getFreeHeap(),
                  ESP.getMinFreeHeap(), sdReady, pmd.loaded || mon.loaded);
    Serial.println("DONE");
  } else if (line == "STATS") {
    Serial.printf("spec=%d nv=%u com=%u fel=%u ene=%u lim=%u hp=%u coins=%u room=%u desc=%u sd=%d mon=%d bat=%d usb=%d rtc=%u\n",
                  pet.speciesId, pet.level(), pet.fullness, pet.joy, pet.energy,
                  pet.hygiene, pet.health, pet.coins, pet.room, pet.careMistakes, sdReady, mon.loaded,
                  batPercent(), usbPresent(), rtcEpoch());
    Serial.printf("peso=%u fue=%u def=%u vel=%u genes=%u/%u/%u tr=%u/%u/%u baya=%d mem=%u\n",
                  pet.weight, pet.atkStat(), pet.defStat(), pet.speStat(),
                  pet.geneAtk, pet.geneDef, pet.geneSpe,
                  pet.trAtk, pet.trDef, pet.trSpe, pet.berryKnown, pet.memoryHi);
    Serial.printf("shiny=%d streak=%u/%u bond=%u medals=0x%X(%u) nick=%s\n",
                  pet.shiny, pet.streak, pet.bestStreak, pet.bond, pet.medals,
                  pet.totalMedals, pet.nick);
    Serial.println("DONE");
  }
}

// ---------- entrada tactil ----------

bool inPetZone(int16_t x, int16_t y) {
  return x > 110 && x < 356 && y > 95 && y < 310;
}

// el toque se resuelve al LEVANTAR el dedo para distinguir tap de deslizar
void handleTouch() {
  static uint32_t lastPoll = 0;
  if (millis() - lastPoll < 10) return;  // 100 Hz para captar deslizamientos cortos
  lastPoll = millis();
  // solo tocamos el bus si el chip aviso por INT o si el dedo sigue abajo (hay
  // que detectar el levantamiento). Leer el CST9217 dormido se colgaba ~1s y
  // congelaba el loop entero; SensorLib no respeta el timeout de Wire.
  if (!gTouchIrq && !wasPressed) return;
  gTouchIrq = false;
  int16_t x, y;
  bool pressed = touch.getPoint(&x, &y, 1) > 0;

  // saco de entrenamiento: cada toque cuenta al instante (aporrear rapido)
  if (sackOpen) {
    if (pressed && !wasPressed) {
      lastInteract = millis();
      tX0 = tXl = x;
      tY0 = tYl = y;
      tStart = millis();
      sackTap();
    } else if (pressed) {
      tXl = x;
      tYl = y;
    } else if (wasPressed && tY0 - tYl >= touchSensitivity) {
      sackOpen = false;
      gameMenuOpen = true;
    }
    wasPressed = pressed;
    return;
  }

  if (pressed && !wasPressed) {  // empieza el gesto
    tX0 = tXl = x;
    tY0 = tYl = y;
    tStart = millis();
    holdFired = false;
    swallowGesture = (dimStage > 0) || screenOff;  // si estaba a oscuras, solo despierta
    screenOff = false;
    lastInteract = millis();
  } else if (pressed) {  // sigue apoyado
    tXl = x;
    tYl = y;
    // pulsacion larga sin moverse sobre el bicho -> dialogo de soltar
    if (!holdFired && !swallowGesture && !galleryOpen && !cardOpen && !kbOpen && !clockOpen &&
        !gameOpen && !memoryOpen && !gameMenuOpen && !worldOpen && !shopOpen && !warehouseOpen && !decorOpen && !battleOpen && !feedMenuUntil && !toyMenuUntil && millis() - tStart > 3000 &&
        abs(tXl - tX0) < 30 && abs(tYl - tY0) < 30 && inPetZone(tX0, tY0) &&
        !pet.isEgg() && !confirmUntil && !pet.ceremony) {
      confirmUntil = millis() + 10000;
      holdFired = true;
    }
  } else if (wasPressed) {  // levanta el dedo: resolver gesto
    lastInteract = millis();
    int dx = tXl - tX0, dy = tYl - tY0;
    uint32_t dt = millis() - tStart;
    if (!holdFired && !swallowGesture) {
      int adx = abs(dx), ady = abs(dy);
      if (adx >= touchSensitivity && adx * 10 > ady * 11 && dt < 1200) onSwipe(dx > 0 ? 1 : -1);
      else if (ady >= touchSensitivity && ady * 10 > adx * 11 && dt < 1200) onSwipeV(dy > 0 ? 1 : -1);
      else if (dt < 1600 && adx <= 48 && ady <= 48) onTap(tX0, tY0);
    }
  }
  wasPressed = pressed;
}

// deslizar vertical: abre/cierra la ficha del bicho
void openClock();  // prototipo

void onSwipeV(int dir) {
  if (pet.awaitingStarter()) return;  // bloqueado durante la eleccion de inicial
  if (pet.ceremony) return;
  if (dir < 0) {  // 向上滑动：返回当前页面的上一级
    if (wifiPickerOpen) { wifiPickerOpen = false; return; }
    if (kbOpen && kbMode) { kbOpen = false; clockOpen = true; return; }
    if (clockOpen) { clockOpen = false; return; }
    if (shopOpen) {
      if (shopDetailOpen) { shopDetailOpen = false; economyMsgUntil = 0; return; }
      if (shopCategory >= 0) { shopCategory = -1; shopScroll = 0; economyMsgUntil = 0; return; }
      closeShop();
      return;
    }
    if (galleryOpen) {
      if (galleryDetail) { galleryDetail = 0; galleryPmd.unload(); galleryDirty = true; }
      else { galleryOpen = false; galleryPmd.unload(); }
      return;
    }
    if (travelOpen) { travelOpen = false; warehouseOpen = true; return; }
    if (warehouseOpen) { warehouseOpen = false; worldOpen = true; return; }
    if (decorOpen) { decorOpen = false; worldOpen = true; return; }
    if (worldOpen) { worldOpen = false; return; }
    if (memoryOpen) { memoryOpen = false; gameMenuOpen = true; return; }
    if (battleOpen) { battleOpen = false; wildPmd.unload(); gameMenuOpen = true; return; }
    if (gameOpen) { gameOpen = false; gameMenuOpen = true; return; }
    if (sackOpen) { sackOpen = false; gameMenuOpen = true; return; }
    if (gameMenuOpen) { gameMenuOpen = false; return; }
    if (kbOpen) {
      kbOpen = false;
      if (kbMode) { wifiPickerOpen = true; return; }
      cardOpen = true;
      return;
    }
    if (cardOpen) { cardOpen = false; return; }
    if (!pet.isEgg() && !confirmUntil && !feedMenuUntil && !toyMenuUntil) {
      cardOpen = true;
      cardPage = 0;
    }
  } else if (dir > 0) {                    // 向下滑动：主页面打开设置
    if (gameOpen || memoryOpen || gameMenuOpen || worldOpen || shopOpen || warehouseOpen || decorOpen || galleryOpen || kbOpen || sackOpen || battleOpen || travelOpen || cardOpen || feedMenuUntil || toyMenuUntil) return;
    if (!confirmUntil && !feedMenuUntil && !toyMenuUntil && !pet.isEgg()) openClock();
  } else if (!pet.isEgg() && !confirmUntil && !feedMenuUntil && !toyMenuUntil) {
    cardOpen = true;                // deslizar arriba: ficha
    cardPage = 0;
  }
}

// deslizar: dir +1 = hacia la derecha. En la pantalla principal, derecha abre
// la Pokedex y izquierda abre el menu de juegos; dentro de la Pokedex los
// gestos laterales cambian de pagina.
void onSwipe(int dir) {
  if (pet.awaitingStarter()) return;  // bloqueado durante la eleccion de inicial
  if (battleOpen) {
    if (dir > 0) { battleOpen = false; wildPmd.unload(); }
    return;
  }
    if (gameOpen || memoryOpen || gameMenuOpen || worldOpen || decorOpen || warehouseOpen || kbOpen || clockOpen || battleOpen || feedMenuUntil || toyMenuUntil) return;
  if (travelOpen) return;
  if (shopOpen) {
    if (shopDetailOpen) {
      // 商品详情像图鉴一样左右滑动浏览，底部只保留返回箭头。
      int next = (int)shopDetailSlot + (dir < 0 ? 1 : -1);
      if (next < 0) next = SHOP_ITEMS_PER_CATEGORY - 1;
      if (next >= SHOP_ITEMS_PER_CATEGORY) next = 0;
      shopDetailSlot = (uint8_t)next;
      shopDetailQty = 1;
      economyMsgUntil = 0;
      sfxPlay(SFX_TAP);
      return;
    }
    if (shopFromGallery && dir > 0) {
      shopOpen = false;
      shopSession++;
      shopFromGallery = false;
      galleryOpen = true;
      galleryDetail = 0;
      galleryPmd.unload();
      galleryDirty = true;
    } else {
      if (shopCategory < 0) return;
      int next = (int)shopScroll + (dir < 0 ? 1 : -1);
      int maxScroll = SHOP_ITEMS_PER_CATEGORY > 5 ? SHOP_ITEMS_PER_CATEGORY - 5 : 0;
      if (next < 0) next = 0;
      if (next > maxScroll) next = maxScroll;
      if (next != shopScroll) {
        shopScroll = (uint8_t)next;
        economyMsgUntil = 0;
        sfxPlay(SFX_TAP);
      }
    }
    return;
  }
  if (cardOpen) {  // dentro de la ficha: cambiar entre las 5 paginas
    int p = (int)cardPage + (dir > 0 ? -1 : 1);  // izquierda avanza
    cardPage = p < 0 ? 0 : (p > 4 ? 4 : p);
    return;
  }
  if (!galleryOpen) {
    if (!pet.ceremony && !confirmUntil) {
      if (dir > 0) {
        galleryOpen = true;
        galleryPage = 0;
        galleryDetail = 0;
        galleryDirty = true;
      } else {
        openGameMenu();
      }
    }
    return;
  }
  if (galleryDetail) {  // en detalle: volver a la rejilla
    galleryDetail = 0;
    galleryPmd.unload();
    galleryDirty = true;
    return;
  }
  // Los gestos laterales se reservan para pasar paginas; la tienda tiene
  // un boton fijo en la parte inferior para no robar el gesto de pagina.
  int np = galleryPage + (dir < 0 ? 1 : -1);  // izquierda avanza pagina
  if (np < 0) {                // retroceder desde la primera = salir
    galleryOpen = false;
    galleryPmd.unload();
    return;
  }
  if (np > 9) np = 9;
  if (np != galleryPage) {
    galleryPage = np;
    galleryDirty = true;
  }
}

void onTap(int16_t x, int16_t y) {
  // Serial.printf("TOUCH %d %d\n", x, y);  // diagnostico (silenciado: satura el log)
  if (pet.awaitingStarter()) {  // primera partida: elegir inicial
    for (int i = 0; i < 3; i++) {
      int ry = STARTER_ROW_Y + i * (STARTER_ROW_H + STARTER_ROW_GAP);
      if (x >= 70 && x <= 396 && y >= ry && y <= ry + STARTER_ROW_H) {
        pet.chooseStarter(STARTER_DEX[i]);
        sfxPlay(SFX_TAP);
        break;
      }
    }
    return;
  }
  if (warehouseOpen) {
    warehouseTap(x, y);
    return;
  }
  if (wifiPickerOpen) {
    wifiPickerTap(x, y);
    return;
  }
  if (pet.sleeping) {
    // 睡眠时触摸只短暂微亮；只有月亮/灯光图标才能真正唤醒。
    sleepTouchUntil = millis() + 5000;
    screenOff = false;
    lastInteract = millis();
    sfxPlay(SFX_TAP);
    if (x >= buttons[2].cx - BTN_HIT && x <= buttons[2].cx + BTN_HIT &&
        y >= buttons[2].cy - BTN_HIT && y <= buttons[2].cy + BTN_HIT) {
      pet.toggleLight();
      sleepTouchUntil = 0;
    }
    return;
  }
  if (galleryOpen) {
    galleryTap(x, y);
    return;
  }
  if (kbOpen) {
    keyboardTap(x, y);
    return;
  }
  if (clockOpen) {
    clockTap(x, y);
    return;
  }
  if (travelOpen) {
    return;
  }
  if (shopOpen) {
    shopTap(x, y);
    return;
  }
  if (decorOpen) {
    decorTap(x, y);
    return;
  }
  if (worldOpen) {
    worldTap(x, y);
    return;
  }
  if (memoryOpen) {
    memoryTap(x, y);
    return;
  }
  if (battleOpen) {
    battleTap(x, y);
    return;
  }
  if (gameMenuOpen) {
    gameMenuTap(x, y);
    return;
  }
  if (pet.ceremony) return;  // durante la despedida no hay botones
  if (cardOpen) {
    if (cardPage == 0 && y < 84) openKeyboard();  // tocar el nombre = renombrar
    else if (cardPage == 1 && y >= 318 && y <= 366 && x >= 96 && x <= 370) {
      cardOpen = false;            // boton ENTRENAR FUERZA
      startSack();
    } else if (cardPage == 4 && y >= 74 && y <= 334) {
      uint8_t slot = (uint8_t)((y - 74) / 50);
      if (slot < EQUIP_SLOT_COUNT) pet.unequipSlot(slot);
    } else {
      cardOpen = false;
    }
    return;
  }
  if (gameOpen) {
    gameTap(x, y);
    return;
  }
  // Tocar la franja superior abre el mapa de salas y la tienda.
  if (!pet.isEgg() && y < 112) {
    if (x >= 300) {
      warehouseOpen = true;
      shopPage = 0;
      sfxPlay(SFX_TAP);
    } else {
      openWorldMenu();
    }
    return;
  }
  if (choiceKind) {          // dialogo de decision: boton accion (arriba) / mantener (abajo)
    bool b1 = (x >= 93 && x <= 373 && y >= 206 && y <= 258);  // accion
    bool b2 = (x >= 93 && x <= 373 && y >= 268 && y <= 320);  // mantener / quedaros
    if (choiceKind == 1) {                 // evolucion
      if (b1) { int16_t old = pet.speciesId; pet.evolve(); evoPmd.load(old, pet.shiny); }
      else if (b2) pet.declineEvolve();
    } else if (choiceKind == 2) {          // despedida
      if (b1) pet.startFarewell();
      else if (b2) pet.declineFarewell();
    }
    choiceKind = 0;
    return;
  }
  if (confirmUntil) {        // dialogo "soltar?": SI / NO
    if (millis() < confirmUntil && x >= 118 && x <= 218 && y >= 252 && y <= 304) {
      pet.release();
    }
    confirmUntil = 0;
    return;
  }
  if (feedMenuUntil || toyMenuUntil) {       // selector de comida / juguetes
    bool toyMenu = toyMenuUntil != 0;
    uint32_t until = toyMenu ? toyMenuUntil : feedMenuUntil;
    if (millis() < until && y >= 276 && y <= 358 && x >= 74 && x <= 392) {
      int item = (x - 86) / 76;
      uint8_t count = toyMenu ? toyCount : feedCount;
      if (item >= 0 && item < count) {
        uint8_t category = toyMenu ? SHOP_CAT_TOY : SHOP_CAT_FOOD;
        uint8_t slot = toyMenu ? toySlots[item] : feedSlots[item];
        if (pet.useShopProduct(category, slot)) sfxPlay(toyMenu ? SFX_PLAY : SFX_EAT);
      }
    }
    feedMenuUntil = 0;
    toyMenuUntil = 0;
    return;
  }
  if (pet.isEgg()) {
    pet.eggTap();
    sfxPlay(SFX_TAP);
    return;
  }
  // boton de evolucion: abre el dialogo evolucionar/mantener
  if (pet.wantEvolveButton() && x >= EVO_BTN_X && x <= EVO_BTN_X + EVO_BTN_W &&
      y >= EVO_BTN_Y && y <= EVO_BTN_Y + EVO_BTN_H) {
    choiceKind = 1; choiceUntil = millis() + 12000;
    return;
  }
  // botones de final (mismo recuadro): escapada directa; despedida abre dialogo
  if (x >= FAR_BTN_X && x <= FAR_BTN_X + FAR_BTN_W &&
      y >= FAR_BTN_Y && y <= FAR_BTN_Y + FAR_BTN_H) {
    if (pet.canRunawayNow()) { pet.startRunaway(); return; }
    if (pet.wantFarewellButton()) { choiceKind = 2; choiceUntil = millis() + 12000; return; }
  }
  for (int i = 0; i < 4; i++) {
    int dx = x - buttons[i].cx, dy = y - buttons[i].cy;
    if (dx * dx + dy * dy <= BTN_HIT * BTN_HIT) {
      Serial.printf("BTN %d\n", i);
      sfxPlay(SFX_TAP);
      if (i == 0) {
        if (!pet.sleeping) feedMenuUntil = millis() + 6000;
      } else if (i == 1) {
        if (!pet.sleeping) toyMenuUntil = millis() + 6000;
      } else if (i == 2) {
        pet.toggleLight();
      } else {
        startBath();
      }
      return;
    }
  }
  // tocar al bicho = caricia
  if (inPetZone(x, y)) {
    Serial.println("PET");
    pet.caress();
    if (!pet.sleeping) sfxPlay(SFX_HEART);
  }
}

// ---------- render ----------

bool gNight = false;  // noche real (por hora) o durmiendo: lo fija render()
uint16_t inkColor() { return gNight ? UI_INK_NIGHT : UI_INK; }

// ---------- escena de fondo: bioma del tipo + hora real del RTC ----------

#define C565(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define HORIZON 232  // linea donde el cielo se encuentra con el suelo

uint16_t lerp565(uint16_t a, uint16_t b, int i, int n) {
  if (n <= 0) return a;
  int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
  int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
  return (uint16_t)((((ar + (br - ar) * i / n) << 11)) |
                    (((ag + (bg - ag) * i / n) << 5)) | (ab + (bb - ab) * i / n));
}

// hora del dia 0-23 (de la hora real cacheada cada 30s; 13 si no hay reloj)
int sceneHour() {
  uint32_t e = pet.lastSeenEpoch;
  return e ? (int)((e / 3600) % 24) : 13;
}

// suelo de cada bioma de dia (de noche se mezcla hacia el azul nocturno)
static const uint16_t BIOME_SOIL[6] = {
  C565(0x7e, 0xc0, 0x7f),  // 0 pradera
  C565(0xdc, 0xca, 0x94),  // 1 playa (arena)
  C565(0x4f, 0x8a, 0x55),  // 2 bosque
  C565(0x8a, 0x55, 0x44),  // 3 volcan
  C565(0xa8, 0x90, 0x6a),  // 4 montana
  C565(0xe6, 0xee, 0xf5),  // 5 nieve
};

void drawClouds(uint32_t now, uint16_t col) {
  for (int k = 0; k < 2; k++) {
    int cx = (int)((now / 50 + k * 250) % 560) - 40;
    int cy = 70 + k * 34;
    gfx->fillCircle(cx, cy, 16, col);
    gfx->fillCircle(cx + 18, cy + 3, 13, col);
    gfx->fillCircle(cx - 15, cy + 4, 12, col);
  }
}

static void drawPixelFlower(int x, int y, uint16_t petal, uint16_t stem) {
  gfx->fillRect(x, y + 8, 3, 16, stem);
  gfx->fillRect(x - 7, y + 7, 7, 7, petal);
  gfx->fillRect(x + 3, y + 7, 7, 7, petal);
  gfx->fillRect(x - 2, y + 2, 7, 7, petal);
  gfx->fillRect(x - 2, y + 10, 7, 6, C565(0xff, 0xd0, 0x43));
}

static void drawPixelTree(int x, int y, uint16_t trunk, uint16_t leaves) {
  gfx->fillRect(x - 6, y + 34, 12, 54, trunk);
  gfx->fillRect(x - 18, y + 26, 36, 12, trunk);
  gfx->fillCircle(x, y + 10, 27, leaves);
  gfx->fillCircle(x - 22, y + 26, 19, leaves);
  gfx->fillCircle(x + 22, y + 28, 20, leaves);
  gfx->fillRect(x - 10, y + 2, 20, 6, lerp565(leaves, UI_WHITE, 2, 10));
}

static void drawRoomDetails(uint8_t biome, uint32_t now, bool night) {
  uint8_t r = pet.room < 4 ? pet.room : 0;
  uint16_t shade = night ? C565(0x24, 0x30, 0x48) : C565(0x3e, 0x6f, 0x55);
  uint16_t light = night ? C565(0xd0, 0xd8, 0x9a) : C565(0xff, 0xe0, 0x8a);

  // Distinct landmarks make each room read as a place, not a flat colour.
  if (r == WORLD_HOME) {
    uint16_t wall = night ? C565(0x32, 0x35, 0x4a) : C565(0xff, 0xe2, 0xc6);
    uint16_t roof = night ? C565(0x53, 0x3d, 0x5b) : C565(0xf0, 0x8a, 0x7f);
    uint16_t trim = night ? C565(0x8a, 0x9b, 0xb7) : C565(0x9a, 0x62, 0x68);
    gfx->fillRoundRect(24, 172, 112, 76, 14, wall);
    gfx->drawRoundRect(24, 172, 112, 76, 14, trim);
    gfx->fillTriangle(18, 176, 80, 126, 142, 176, roof);
    gfx->drawLine(18, 176, 80, 126, trim);
    gfx->drawLine(80, 126, 142, 176, trim);
    gfx->fillRoundRect(61, 202, 32, 46, 9, C565(0x7b, 0x50, 0x62));
    gfx->fillCircle(86, 224, 3, C565(0xff, 0xd0, 0x43));
    gfx->fillRoundRect(36, 188, 28, 24, 6, night ? C565(0x2d, 0x55, 0x70) : C565(0x8e, 0xd5, 0xe8));
    gfx->fillRoundRect(96, 188, 28, 24, 6, night ? C565(0x2d, 0x55, 0x70) : C565(0x8e, 0xd5, 0xe8));
    gfx->drawRect(49, 188, 3, 24, trim); gfx->drawRect(109, 188, 3, 24, trim);
    gfx->fillRect(107, 133, 13, 24, trim);
    drawPixelFlower(32, 234, C565(0xff, 0x8e, 0x9a), shade);
    drawPixelFlower(128, 238, C565(0x8e, 0xb8, 0xff), shade);
    gfx->fillRect(183, 272, 100, 18, night ? C565(0x42, 0x31, 0x45) : C565(0xc5, 0x78, 0x70));
    gfx->drawRoundRect(183, 272, 100, 18, 8, shade);
  } else if (r == WORLD_PARK) {
    drawPixelTree(70, 132, C565(0x72, 0x45, 0x2d), night ? C565(0x1d, 0x45, 0x35) : C565(0x39, 0x9b, 0x62));
    drawPixelTree(402, 144, C565(0x72, 0x45, 0x2d), night ? C565(0x1d, 0x45, 0x35) : C565(0x39, 0x9b, 0x62));
    gfx->fillRect(145, 220, 176, 8, shade);
    gfx->fillRect(155, 228, 8, 33, shade);
    gfx->fillRect(303, 228, 8, 33, shade);
    gfx->fillRect(146, 202, 174, 10, shade);
    for (int x = 28; x < 450; x += 52) drawPixelFlower(x, 264, light, shade);
  } else if (r == WORLD_BEACH) {
    uint16_t palm = C565(0x76, 0x4d, 0x2d);
    gfx->fillRect(70, 138, 12, 112, palm);
    gfx->fillTriangle(76, 146, 34, 126, 70, 128, C565(0x39, 0xa5, 0x6c));
    gfx->fillTriangle(76, 148, 116, 126, 84, 130, C565(0x39, 0xa5, 0x6c));
    gfx->fillTriangle(76, 150, 55, 106, 75, 127, C565(0x4a, 0xb8, 0x75));
    gfx->fillRoundRect(350, 224, 70, 30, 8, C565(0xd8, 0x9e, 0x56));
    gfx->fillTriangle(350, 224, 385, 190, 420, 224, C565(0xe5, 0xb6, 0x67));
    gfx->fillRect(382, 224, 6, 30, shade);
    gfx->fillRect(166, 265, 38, 18, C565(0xd2, 0x8c, 0x45));
    gfx->fillTriangle(166, 265, 185, 245, 204, 265, C565(0xe7, 0xb0, 0x5b));
    gfx->fillRect(180, 257, 5, 8, shade);
    gfx->fillRect(190, 253, 5, 12, shade);
  } else {
    drawPixelTree(54, 130, C565(0x64, 0x3e, 0x2a), night ? C565(0x1b, 0x3b, 0x31) : C565(0x2d, 0x7b, 0x4f));
    drawPixelTree(414, 124, C565(0x64, 0x3e, 0x2a), night ? C565(0x1b, 0x3b, 0x31) : C565(0x2d, 0x7b, 0x4f));
    gfx->fillRoundRect(118, 254, 112, 24, 12, C565(0x63, 0x42, 0x2b));
    gfx->fillCircle(132, 266, 15, C565(0x87, 0x58, 0x36));
    gfx->fillCircle(218, 266, 15, C565(0x87, 0x58, 0x36));
    drawPixelFlower(300, 264, C565(0xd5, 0x6c, 0x8d), shade);
    drawPixelFlower(360, 270, C565(0x8d, 0x79, 0xd5), shade);
  }

  uint8_t placed = pet.roomDecor();
  if (placed & 1) {
    int bx = r == WORLD_BEACH ? 272 : 92;
    int by = r == WORLD_BEACH ? 268 : 266;
    uint16_t ball = C565(0xe9, 0x4b, 0x45);
    gfx->fillCircle(bx, by, 14, UI_WHITE);
    gfx->fillCircle(bx, by, 11, ball);
    gfx->fillRect(bx - 11, by - 2, 22, 4, UI_WHITE);
    gfx->fillCircle(bx, by, 4, UI_WHITE);
    gfx->drawCircle(bx, by, 14, shade);
  }
  if (placed & 2) {
    drawPixelFlower(r == WORLD_FOREST ? 252 : 372, 264, C565(0xff, 0x8e, 0x9a), shade);
    drawPixelFlower(r == WORLD_FOREST ? 282 : 402, 270, C565(0x8e, 0xb8, 0xff), shade);
  }
  if (placed & 4) {
    int tx = r == WORLD_BEACH ? 278 : 350;
    gfx->fillTriangle(tx, 250, tx + 34, 206, tx + 68, 250, C565(0xe0, 0x73, 0x59));
    gfx->fillTriangle(tx + 8, 250, tx + 34, 214, tx + 60, 250, C565(0xf4, 0xc1, 0x66));
    gfx->fillRect(tx + 30, 236, 10, 14, C565(0x5b, 0x3d, 0x4e));
  }
  if (placed & 8) {
    int lx = r == WORLD_HOME ? 390 : 430;
    gfx->fillRect(lx, 194, 5, 60, shade);
    gfx->fillCircle(lx + 2, 185, 18, light);
    gfx->fillCircle(lx + 2, 185, 10, C565(0xff, 0xf4, 0xb0));
  }
  if (placed & 16) {
    int dx = r == WORLD_HOME ? 304 : 112;
    int dy = 258;
    gfx->fillRoundRect(dx - 22, dy - 12, 44, 28, 8, C565(0xd2, 0x68, 0x66));
    gfx->fillRect(dx - 18, dy - 5, 36, 4, C565(0xff, 0xd0, 0x84));
    gfx->fillRect(dx - 5, dy - 12, 10, 28, shade);
    gfx->drawRoundRect(dx - 22, dy - 12, 44, 28, 8, shade);
  }
  if (placed & 32) {
    int bx = r == WORLD_PARK ? 338 : 154;
    gfx->fillRect(bx - 24, 260, 22, 22, C565(0xe8, 0x68, 0x58));
    gfx->fillRect(bx + 2, 252, 22, 30, C565(0x66, 0x9b, 0xe8));
    gfx->fillRect(bx - 8, 244, 22, 22, C565(0xf0, 0xc3, 0x55));
    gfx->drawRect(bx - 24, 260, 22, 22, shade);
    gfx->drawRect(bx + 2, 252, 22, 30, shade);
    gfx->drawRect(bx - 8, 244, 22, 22, shade);
  }
  if (placed & 64) {
    int tx = r == WORLD_FOREST ? 308 : 70;
    int ty = 274;
    gfx->fillRect(tx - 30, ty - 18, 60, 22, C565(0x4a, 0x9c, 0xd1));
    gfx->fillRect(tx - 20, ty - 32, 24, 14, C565(0xf0, 0x76, 0x5b));
    gfx->fillRect(tx + 6, ty - 32, 20, 14, C565(0xf0, 0xc3, 0x55));
    gfx->fillCircle(tx - 18, ty + 5, 9, shade);
    gfx->fillCircle(tx + 18, ty + 5, 9, shade);
    gfx->fillCircle(tx - 18, ty + 5, 4, C565(0xdc, 0xe6, 0xe9));
    gfx->fillCircle(tx + 18, ty + 5, 4, C565(0xdc, 0xe6, 0xe9));
  }
  if (placed & 128) {
    int kx = r == WORLD_BEACH ? 300 : 392;
    int ky = r == WORLD_BEACH ? 136 : 154;
    gfx->fillTriangle(kx, ky - 28, kx - 24, ky, kx, ky + 20, C565(0xff, 0x70, 0x86));
    gfx->fillTriangle(kx, ky - 28, kx + 24, ky, kx, ky + 20, C565(0x76, 0xb8, 0xf0));
    gfx->drawLine(kx, ky - 28, kx, ky + 20, shade);
    gfx->drawLine(kx - 24, ky, kx + 24, ky, shade);
    gfx->drawLine(kx, ky + 20, kx - 10, ky + 36, shade);
    gfx->fillCircle(kx - 12, ky + 38, 4, C565(0xff, 0xd0, 0x43));
  }
  (void)biome;
  (void)now;
}

void drawScene(uint8_t biome, uint32_t now, bool night) {
  int h = sceneHour();
  uint16_t top, bot;
  if (night)            { top = C565(0x0c, 0x12, 0x24); bot = C565(0x1e, 0x26, 0x46); }
  else if (h < 8)       { top = C565(0xd1, 0x6a, 0x86); bot = C565(0xf3, 0xb8, 0x7c); }  // amanecer
  else if (h < 18)      { top = C565(0x8f, 0xc8, 0xea); bot = C565(0xdc, 0xee, 0xe6); }  // dia
  else                  { top = C565(0xc7, 0x5a, 0x4a); bot = C565(0xf0, 0xae, 0x64); }  // atardecer

  // cielo en bandas
  for (int y = 0; y < HORIZON; y += 8)
    gfx->fillRect(0, y, 466, 8, lerp565(top, bot, y, HORIZON));

  // sol o luna
  if (night) {
    gfx->fillCircle(360, 78, 24, C565(0xe8, 0xee, 0xf5));
    gfx->fillCircle(370, 72, 22, lerp565(top, bot, 78, HORIZON));  // creciente
    for (auto &st : STARS) gfx->fillRect(st[0], st[1], 4, 4, UI_WHITE);
  } else if (h < 18) {
    gfx->fillCircle(360, 84, 26, h < 8 ? C565(0xff, 0xd9, 0x8a) : C565(0xff, 0xe7, 0x9f));
    drawClouds(now, C565(0xff, 0xff, 0xff));
  } else {
    gfx->fillCircle(233, HORIZON - 6, 34, C565(0xff, 0xf1, 0xc8));  // sol poniente
  }

  // mar de la playa: una franja de agua sobre la arena
  uint16_t soil = BIOME_SOIL[biome < 6 ? biome : 0];
  if (night) soil = lerp565(soil, C565(0x16, 0x1c, 0x30), 9, 16);
  if (biome == 1) {
    uint16_t sea = night ? C565(0x1c, 0x34, 0x52) : C565(0x4f, 0x96, 0xc4);
    gfx->fillRect(0, HORIZON - 26, 466, 26, sea);
    for (int i = 0; i < 3; i++) {
      int wy = HORIZON - 22 + i * 7;
      uint16_t fc = night ? C565(0x3a, 0x58, 0x78) : C565(0xbf, 0xe6, 0xf5);
      gfx->fillRect(60 + ((now / 60 + i * 30) % 60), wy, 26, 2, fc);
      gfx->fillRect(300 - ((now / 60 + i * 20) % 60), wy, 26, 2, fc);
    }
  }

  // suelo
  gfx->fillRect(0, HORIZON, 466, 466 - HORIZON, soil);
  uint16_t hill = lerp565(soil, night ? C565(0x0c, 0x12, 0x24) : C565(0xff, 0xff, 0xff), 3, 16);
  gfx->fillRoundRect(-60, HORIZON - 14, 586, 60, 30, hill);

  // detalles del bioma
  uint16_t dk = lerp565(soil, C565(0x10, 0x18, 0x20), night ? 11 : 7, 16);
  if (biome == 2) {  // bosque: coniferas en silueta
    for (int tx : { 60, 150, 360, 416 }) {
      gfx->fillTriangle(tx, HORIZON - 46, tx - 16, HORIZON, tx + 16, HORIZON, dk);
      gfx->fillTriangle(tx, HORIZON - 60, tx - 12, HORIZON - 28, tx + 12, HORIZON - 28, dk);
    }
  } else if (biome == 3) {  // volcan: rocas y brasas
    gfx->fillTriangle(70, HORIZON, 40, HORIZON + 30, 100, HORIZON + 30, dk);
    gfx->fillTriangle(400, HORIZON + 4, 372, HORIZON + 30, 430, HORIZON + 30, dk);
    if (!night)
      for (int e = 0; e < 4; e++)
        gfx->fillRect(120 + e * 70, HORIZON + 8 + (e % 2) * 6, 4, 4, C565(0xff, 0x9b, 0x3a));
  } else if (biome == 4) {  // montana: cumbres al fondo
    gfx->fillTriangle(140, HORIZON - 50, 60, HORIZON, 220, HORIZON, dk);
    gfx->fillTriangle(330, HORIZON - 38, 250, HORIZON, 410, HORIZON, dk);
  } else if (biome == 5 && !night) {  // nieve: copos cayendo
    for (int f = 0; f < 10; f++) {
      int fx = (f * 53 + now / 40) % 466;
      int fy = (f * 90 + now / 18) % HORIZON;
      gfx->fillRect(fx, fy, 3, 3, UI_WHITE);
    }
  } else if (biome == 0) {  // pradera: matas de hierba
    for (int gx : { 80, 175, 300, 395 })
      for (int b = -1; b <= 1; b++)
        gfx->fillRect(gx + b * 5, HORIZON + 6, 2, 8 + (b == 0 ? 4 : 0), dk);
  }
  drawRoomDetails(biome, now, night);
}

// primera partida: elige inicial entre Bulbasaur / Charmander / Squirtle
void renderStarterSelect() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  const char *t = T(S_CHOOSE_STARTER);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - uiTextWidth(t, 6) / 2, 68);
  gfx->print(t);
  for (int i = 0; i < 3; i++) {
    int16_t d = STARTER_DEX[i];
    const DexEntry &de = DEX_TBL[d];
    int ry = STARTER_ROW_Y + i * (STARTER_ROW_H + STARTER_ROW_GAP);
    gfx->fillRoundRect(70, ry, 326, STARTER_ROW_H, 14, lerp565(de.accent, UI_WHITE, 6, 8));
    gfx->drawRoundRect(70, ry, 326, STARTER_ROW_H, 14, de.accent);
    const uint8_t *th = thumbs.get(d);     // miniatura del inicial (si la SD esta lista)
    if (th) drawThumb(th, 76, ry - 5, 3, false);
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(3);
    gfx->setCursor(178, ry + 24);
    gfx->print(dexName(d));
  }
  gfx->flush();
}

void render() {
  if (pet.awaitingStarter()) {  // primera partida: elegir inicial (prioridad total)
    renderStarterSelect();
    return;
  }
  if (galleryOpen) {
    renderGallery();
    return;
  }
  if (gameMenuOpen) {
    renderGameMenu();
    return;
  }
  if (battleOpen) {
    renderBattle();
    return;
  }
  if (memoryOpen) {
    renderMemoryGame();
    return;
  }
  if (travelOpen) {
    renderTravel();
    return;
  }
  if (shopOpen) {
    renderShop();
    return;
  }
  if (warehouseOpen) {
    renderWarehouse();
    return;
  }
  if (decorOpen) {
    renderDecor();
    return;
  }
  if (worldOpen) {
    renderWorldMenu();
    return;
  }
  if (gameOpen) {
    renderGame();
    return;
  }
  if (sackOpen) {
    renderSack();
    return;
  }
  if (wifiPickerOpen) {
    renderWifiPicker();
    return;
  }
  if (kbOpen) {
    renderKeyboard();
    return;
  }
  if (clockOpen) {
    renderClock();
    return;
  }
  if (cardOpen) {
    renderCard();
    return;
  }
  int h = sceneHour();
  gNight = pet.sleeping || h < 6 || h >= 20;
  // drawScene cubre los 466x466 completos: sin fillScreen(NEGRO) previo para
  // que un flush DMA solapado nunca capture negro a medias (anti-parpadeo)
  drawScene(roomBiome(), millis(), gNight);

  if (pet.ceremony) {
    const DexEntry &d = DEX_TBL[pet.speciesId];
    const char *msg = (pet.ceremony == CER_FAREWELL) ? T(S_FAREWELL)
                      : (pet.ceremony == CER_RUNAWAY) ? T(S_RUNAWAY)
                                                      : T(S_GOODBYE);
    drawHeader(dexName(pet.speciesId), d.accent, msg);
    drawCeremony();
    gfx->flush();
    return;
  }

  if (pet.isEgg()) {
    drawHeader(T(S_EGG_HDR), inkColor(), eggMsg());
    int s = 5, x = CX - 16 * s, y = PET_CY - 16 * s;
    drawMap(SPR_EGG, SPRITE_H, x, y, s, false);
    if (pet.eggCracks() >= 1)
      for (auto &c : CRACK1) gfx->fillRect(x + c[0] * s, y + c[1] * s, s, s, INK_K);
    if (pet.eggCracks() >= 2)
      for (auto &c : CRACK2) gfx->fillRect(x + c[0] * s, y + c[1] * s, s, s, INK_K);
    if (pet.eggRarity() >= R_RARO) {
      const char *rar = (pet.eggRarity() == R_LEGENDARIO) ? T(S_EGG_LEGEND) : T(S_EGG_RARE);
      gfx->setTextColor(pet.eggRarity() == R_LEGENDARIO ? UI_BAR_WARN : 0x4C98);
      gfx->setTextSize(2);
      gfx->setCursor(CX - uiTextWidth(rar, 6) / 2, 316);
      gfx->print(rar);
    }
    char reg[24];
    snprintf(reg, sizeof(reg), T(S_POKEDEX_FMT), pet.registeredCount());
    gfx->fillRect(0, 312, 466, 154, gNight ? UI_BG_NIGHT : UI_BG_DAY);
    gfx->setTextColor(inkColor());
    gfx->setTextSize(2);
    gfx->setCursor(CX - uiTextWidth(reg, 6) / 2, 348);
    gfx->print(reg);
  } else {
    const DexEntry &d = DEX_TBL[pet.speciesId];
    char name[64];
    const char *base = pet.nick[0] ? pet.nick : dexName(pet.speciesId);
    snprintf(name, sizeof(name), T(S_NAME_FMT), pet.shiny ? "*" : "", base, pet.level());
    drawHeader(name, gNight ? UI_INK_NIGHT : d.accent, statusMsg());
    drawStreakBadge();
    drawPet();
    drawWorldBadge();
    drawBath();
    drawPoops();
    if (poopCleanMsgUntil && millis() < poopCleanMsgUntil) {
      const char *cleanMsg = "便便已清理";
      gfx->fillRoundRect(158, 268, 150, 30, 8, UI_BAR_OK);
      gfx->setTextColor(UI_WHITE); gfx->setTextSize(1);
      gfx->setCursor(CX - uiTextWidth(cleanMsg, 6) / 2, 271);
      gfx->print(cleanMsg);
    }
    // panel inferior: base limpia para barras y botones sobre el paisaje
    gfx->fillRect(0, 312, 466, 154, gNight ? UI_BG_NIGHT : UI_BG_DAY);
    drawBars();
    drawButtons();
    drawCelebration();
    if (pet.wantEvolveButton()) drawEvolveButton();        // CTA rojo: evolucionar
    else if (pet.canRunawayNow()) drawRunawayButton();     // CTA sombrio: escapada (abandono)
    else if (pet.wantFarewellButton()) drawFarewellButton();  // CTA dorado: despedida
  }

  if (pet.sleeping) {
    gfx->setTextColor(UI_INK_NIGHT);
    gfx->setTextSize(3);
    gfx->setCursor(320, 130);
    gfx->print("Zz");
  }

  // selector de comida
  if (feedMenuUntil || toyMenuUntil) {
    bool toyMenu = toyMenuUntil != 0;
    uint32_t until = toyMenu ? toyMenuUntil : feedMenuUntil;
    if (millis() > until) {
      feedMenuUntil = 0;
      toyMenuUntil = 0;
    } else {
      gfx->fillRoundRect(74, 276, 318, 82, 14, UI_WHITE);
      gfx->drawRoundRect(74, 276, 318, 82, 14, inkColor());
      uint8_t *slots = toyMenu ? toySlots : feedSlots;
      uint8_t &count = toyMenu ? toyCount : feedCount;
      uint8_t category = toyMenu ? SHOP_CAT_TOY : SHOP_CAT_FOOD;
      count = 0;
      // 已购买物品优先，避免食品的系统默认项占满选择器。
      uint8_t first = toyMenu ? 0 : 4;
      for (uint8_t slot = first; slot < SHOP_ITEMS_PER_CATEGORY && count < 4; slot++) {
        if (pet.warehouseCount(category, slot)) slots[count++] = slot;
      }
      // 没有购买食品时，补回系统自带的四种基础食品。
      if (!toyMenu) {
        for (uint8_t slot = 0; slot < 4 && count < 4; slot++) {
          slots[count++] = slot;
        }
      }
      if (!count) {
        const char *empty = toyMenu ? "仓库暂无玩具" : "仓库暂无食品";
        gfx->setTextColor(UI_INK); gfx->setTextSize(1);
        gfx->setCursor(CX - uiTextWidth(empty, 6) / 2, 308); gfx->print(empty);
      } else {
        uint8_t shown = count;
        for (uint8_t i = 0; i < shown; i++) {
          int x = 100 + i * 72;
          drawProductIcon(category, slots[i], x + 12, 304, 1);
          gfx->setTextColor(UI_INK); gfx->setTextSize(1);
          const char *name = SHOP_PRODUCTS[category][slots[i]].name;
          gfx->setCursor(x - 10, 330); gfx->print(name);
        }
      }
    }
  }

  // dialogo "soltar?" (pulsacion larga sobre el bicho)
  if (confirmUntil) {
    if (millis() > confirmUntil) {
      confirmUntil = 0;
    } else {
      gfx->fillRoundRect(94, 168, 278, 152, 16, UI_WHITE);
      gfx->drawRoundRect(94, 168, 278, 152, 16, UI_INK);
      char q[64];
      snprintf(q, sizeof(q), T(S_RELEASE_FMT), dexName(pet.speciesId));
      gfx->setTextColor(UI_INK);
      gfx->setTextSize(2);
      gfx->setCursor(CX - uiTextWidth(q, 6) / 2, 196);
      gfx->print(q);
      gfx->fillRoundRect(118, 252, 100, 52, 12, UI_BAR_OK);
      gfx->setTextColor(UI_WHITE);
      gfx->setCursor(118 + (100 - uiTextWidth(T(S_YES), 12)) / 2, 270);
      gfx->print(T(S_YES));
      gfx->fillRoundRect(248, 252, 100, 52, 12, UI_BAR_BAD);
      gfx->setCursor(248 + (100 - uiTextWidth(T(S_NO), 12)) / 2, 270);
      gfx->print(T(S_NO));
    }
  }

  // dialogo de decision (evolucionar/mantener, despedirse/quedaros)
  if (choiceKind) {
    if (millis() > choiceUntil) choiceKind = 0;
    else drawChoiceDialog();
  }

  gfx->flush();
}

// ---------- minijuego: toques con la pokeball ----------

void startGame() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony) return;
  gameOpen = true;
  gameOverUntil = 0;
  gameScore = 0;
  gameMisses = 0;
  gameNewHi = false;
  hitTime = 0;
  gamePetX = 233;
  respawnBall();
}

void respawnBall() {
  ballX = 150 + random(166);
  ballY = 96;
  float sp = 1.6f + gameScore * 0.05f;  // mas viva segun avanzas
  if (sp > 4.0f) sp = 4.0f;
  ballVX = random(2) ? sp : -sp;
  ballVY = 0;
}

void gameTap(int16_t x, int16_t y) {
  if (gameOverUntil) return;
  float dx = ballX - x, dy = ballY - y;
  if (dx * dx + dy * dy < 74 * 74) {  // toque a la bola!
    gameScore++;
    sfxPlay(SFX_PLAY);
    // golpe mas suave: impulso moderado que crece poco a poco con la puntuacion
    float lift = 6.6f + (gameScore > 16 ? 3.5f : gameScore * 0.22f);
    ballVY = -lift;
    ballVX += dx * 0.12f;
    if (ballVX > 6.5f) ballVX = 6.5f;
    if (ballVX < -6.5f) ballVX = -6.5f;
    hitX = ballX;
    hitY = ballY;
    hitTime = millis();
  }
}

void stepGame() {
  float grav = 0.40f + gameScore * 0.013f;  // cae un poco mas rapido cada vez
  if (grav > 0.80f) grav = 0.80f;
  ballVY += grav;
  ballX += ballVX;
  ballY += ballVY;
  // rebote en la pared circular
  float dx = ballX - CX, dy = ballY - CY;
  float d = sqrtf(dx * dx + dy * dy);
  if (d > 205) {
    float nx = dx / d, ny = dy / d;
    float dot = ballVX * nx + ballVY * ny;
    if (dot > 0) {
      ballVX = (ballVX - 2 * dot * nx) * 0.85f;
      ballVY = (ballVY - 2 * dot * ny) * 0.85f;
    }
    ballX = CX + nx * 205;
    ballY = CY + ny * 205;
  }
  if (ballY > 384) {  // al suelo
    if (++gameMisses >= 3) {
      gameNewHi = (gameScore > pet.gameHi);
      pet.playResult(gameScore);  // actualiza el record y da felicidad
      sfxPlay(gameNewHi && gameScore > 0 ? SFX_MEDAL : SFX_LEVEL);
      gameOverUntil = millis() + 4000;
    } else {
      respawnBall();
    }
  }
  // el bicho la sigue por abajo
  float chase = (ballX - gamePetX) * 0.12f;
  if (chase > 7) chase = 7;
  if (chase < -7) chase = -7;
  gamePetX += chase;
}

// ---------- saco de entrenamiento (entrena la fuerza) ----------

void startSack() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony) return;
  sackOpen = true;
  sackUntil = millis() + 10000;
  sackOverUntil = 0;
  sackHits = 0;
  sackShake = 0;
  sackNewHi = false;
}

void sackTap() {
  if (millis() >= sackUntil) return;  // ya termino el tiempo
  sackHits++;
  sackShake = 16;  // sacude el saco
}

void drawGameScene();  // prototipo (definida mas abajo)

void renderSack() {
  uint32_t now = millis();
  drawGameScene();  // fondo del habitat
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;

  // pantalla de resultado
  if (sackOverUntil) {
    if (now > sackOverUntil) { sackOpen = false; return; }
    char b[20];
    snprintf(b, sizeof(b), T(S_HITS_FMT), sackHits);
    gfx->setTextColor(ink);
    gfx->setTextSize(4);
    gfx->setCursor(CX - uiTextWidth(b, 12) / 2, 150);
    gfx->print(b);
    char g[18];
    snprintf(g, sizeof(g), T(S_STR_GAIN_FMT), sackGain);
    gfx->setTextColor(UI_BAR_BAD);
    gfx->setTextSize(3);
    gfx->setCursor(CX - uiTextWidth(g, 9) / 2, 210);
    gfx->print(g);
    gfx->setTextSize(2);
    if (sackNewHi && sackHits > 0) {
      gfx->setTextColor(UI_BAR_WARN);
      gfx->setCursor(CX - uiTextWidth(T(S_NEW_RECORD), 6) / 2, 256);
      gfx->print(T(S_NEW_RECORD));
    } else {
      char r[18];
      snprintf(r, sizeof(r), T(S_RECORD_FMT), pet.strHi);
      gfx->setTextColor(ink);
      gfx->setCursor(CX - uiTextWidth(r, 6) / 2, 256);
      gfx->print(r);
    }
    gfx->flush();
    return;
  }

  // se acabaron los 10 s: aplicar entrenamiento
  if (now >= sackUntil) {
    sackNewHi = (sackHits > pet.strHi);
    sackGain = pet.trainStrength(sackHits);
    sfxPlay(sackNewHi ? SFX_MEDAL : SFX_PLAY);
    sackOverUntil = now + 3500;
    gfx->flush();
    return;
  }

  // aporreo activo
  sackShake *= 0.84f;
  int off = (int)(sackShake * sinf(now * 0.05f));
  int sx = CX + off, top = 86, sy = 150;
  gfx->fillRect(CX - 3, 56, 6, top - 56, ink);          // gancho/cuerda
  gfx->fillRect(sx - 4, top - 30, 8, 34, ink);          // cadena
  gfx->fillRoundRect(sx - 42, top, 84, 150, 26, C565(0xb5, 0x3a, 0x3a));  // saco
  gfx->fillRoundRect(sx - 42, top, 84, 22, 18, C565(0x7e, 0x28, 0x28));   // tapa
  gfx->drawRoundRect(sx - 42, top, 84, 150, 26, ink);
  gfx->fillRect(sx - 42, top + 70, 84, 4, C565(0x7e, 0x28, 0x28));        // costura

  // contador de golpes
  char buf[8];
  snprintf(buf, sizeof(buf), "%u", sackHits);
  gfx->setTextColor(ink);
  gfx->setTextSize(6);
  gfx->setCursor(CX - uiTextWidth(buf, 18) / 2, 268);
  gfx->print(buf);

  gfx->setTextSize(2);
  gfx->setCursor(CX - uiTextWidth(T(S_HIT_FAST), 6) / 2, 322);
  gfx->print(T(S_HIT_FAST));

  // barra de tiempo
  uint32_t left = sackUntil - now;
  int bw = 280, fw = (int)((uint32_t)bw * left / 10000);
  gfx->fillRoundRect(CX - bw / 2, 350, bw, 16, 5, UI_TRACK);
  if (fw > 2) gfx->fillRoundRect(CX - bw / 2, 350, fw, 16, 5, UI_BAR_OK);

  gfx->flush();
}

// fondo del minijuego: hatibat del bicho (cielo por hora + suelo del bioma)
void drawGameScene() {
  int hh = sceneHour();
  bool night = hh < 6 || hh >= 20;
  uint16_t top, bot;
  if (night)       { top = C565(0x0c, 0x12, 0x24); bot = C565(0x1e, 0x26, 0x46); }
  else if (hh < 8) { top = C565(0xd1, 0x6a, 0x86); bot = C565(0xf3, 0xb8, 0x7c); }
  else if (hh < 18){ top = C565(0x8f, 0xc8, 0xea); bot = C565(0xdc, 0xee, 0xe6); }
  else             { top = C565(0xc7, 0x5a, 0x4a); bot = C565(0xf0, 0xae, 0x64); }
  int hor = 376;
  for (int y = 0; y < hor; y += 8)
    gfx->fillRect(0, y, 466, 8, lerp565(top, bot, y, hor));
  if (night)
    for (auto &st : STARS) gfx->fillRect(st[0], st[1], 4, 4, UI_WHITE);
  uint8_t bio = pet.isEgg() ? 0 : DEX_TBL[pet.speciesId].biome;
  uint16_t soil = BIOME_SOIL[bio < 6 ? bio : 0];
  if (night) soil = lerp565(soil, C565(0x16, 0x1c, 0x30), 9, 16);
  gfx->fillRect(0, hor, 466, 466 - hor, soil);
}

static uint8_t roomBiome() {
  if (pet.room == WORLD_BEACH) return 1;
  if (pet.room == WORLD_FOREST) return 2;
  if (pet.room == WORLD_PARK) return 0;
  return pet.isEgg() ? 0 : DEX_TBL[pet.speciesId].biome;
}

// ---------- TamaPetchi: menu y juego de memoria ----------

static const uint16_t MEMORY_COLORS[16] = {
  C565(0xe7, 0x4c, 0x3c), C565(0x34, 0x98, 0xdb), C565(0x2e, 0xcc, 0x71), C565(0xf3, 0x9c, 0x12),
  C565(0x9b, 0x59, 0xb6), C565(0x1a, 0xbc, 0x9c), C565(0xe6, 0x7e, 0x22), C565(0x29, 0x80, 0xb9),
  C565(0x27, 0xae, 0x60), C565(0xc0, 0x39, 0x2b), C565(0x8e, 0x44, 0xad), C565(0x16, 0xa0, 0x85),
  C565(0xd3, 0x54, 0x00), C565(0x7f, 0x8c, 0x8d), C565(0x2c, 0x3e, 0x50), C565(0xf1, 0xc4, 0x0f),
};

void openGameMenu() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony) return;
  gameOpen = false;
  memoryOpen = false;
  gameMenuOpen = true;
  sfxPlay(SFX_TAP);
}

void gameMenuTap(int16_t x, int16_t y) {
  if (x < 52 || x > 414) return;
  if (y >= 84 && y <= 136) {
    gameMenuOpen = false;
    startBattle();
  } else if (y >= 146 && y <= 198) {
    gameMenuOpen = false;
    startGame();
  } else if (y >= 208 && y <= 260) {
    gameMenuOpen = false;
    startMemoryGame();
  } else if (y >= 270 && y <= 322) {
    gameMenuOpen = false;
    startSack();
  }
}

void startMemoryGame() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony) return;
  memorySession++;
  memoryOpen = true;
  memoryPhase = MEM_SHOWING;
  memoryRound = 1;
  memoryStep = 0;
  memoryInput = 0;
  memoryScore = 0;
  memoryFlash = -1;
  memoryWon = false;
  for (uint8_t i = 0; i < 30; i++) memorySequence[i] = (uint8_t)random(16);
  memoryNextMs = millis() + 550;
  memoryFlashUntil = 0;
  memoryResultUntil = 0;
  sfxPlay(SFX_PLAY);
}

static void endMemoryGame(bool won) {
  memoryWon = won;
  memoryPhase = MEM_RESULT;
  memoryFlash = -1;
  memoryResultUntil = millis() + 3500;
  pet.memoryResult((uint8_t)(memoryScore > 255 ? 255 : memoryScore), won);
  sfxPlay(won ? SFX_MEDAL : SFX_DENY);
}

static int memoryCellAt(int16_t x, int16_t y) {
  for (int row = 0; row < 4; row++) {
    int top = MEM_GRID_Y + row * MEM_GRID_STEP;
    if (y < top || y >= top + MEM_CELL) continue;
    for (int col = 0; col < 4; col++) {
      int left = MEM_GRID_X + col * MEM_GRID_STEP;
      if (x >= left && x < left + MEM_CELL) return row * 4 + col;
    }
  }
  return -1;
}

void memoryTap(int16_t x, int16_t y) {
  if (memoryPhase != MEM_INPUT) return;
  int cell = memoryCellAt(x, y);
  if (cell < 0) return;
  memoryFlash = (int8_t)cell;
  memoryFlashUntil = millis() + 140;
  if (cell != memorySequence[memoryInput]) {
    endMemoryGame(false);
    return;
  }
  memoryScore++;
  if (++memoryInput < memoryRound) return;
  if (memoryRound >= 30) {
    endMemoryGame(true);
    return;
  }
  memoryRound++;
  memoryStep = 0;
  memoryInput = 0;
  memoryPhase = MEM_SHOWING;
  memoryNextMs = millis() + 550;
}

static void stepMemoryGame() {
  uint32_t now = millis();
  if (memoryFlash >= 0 && now >= memoryFlashUntil) memoryFlash = -1;
  if (memoryPhase == MEM_RESULT) {
    if (now >= memoryResultUntil) memoryOpen = false;
    return;
  }
  if (memoryPhase != MEM_SHOWING || now < memoryNextMs) return;
  if (memoryStep < memoryRound) {
    memoryFlash = (int8_t)memorySequence[memoryStep++];
    memoryFlashUntil = now + 360;
    memoryNextMs = now + 700;
  } else {
    memoryFlash = -1;
    memoryInput = 0;
    memoryPhase = MEM_INPUT;
  }
}

static void drawNavChevron(int cx, int cy, bool right, uint16_t color) {
  if (right) {
    gfx->fillTriangle(cx + 13, cy, cx - 8, cy - 14, cx - 8, cy + 14, color);
  } else {
    gfx->fillTriangle(cx - 13, cy, cx + 8, cy - 14, cx + 8, cy + 14, color);
  }
}

static void drawSelectCheck(int cx, int cy, uint16_t color) {
  gfx->drawLine(cx - 12, cy, cx - 3, cy + 9, color);
  gfx->drawLine(cx - 3, cy + 9, cx + 14, cy - 11, color);
  gfx->drawLine(cx - 12, cy + 1, cx - 3, cy + 10, color);
  gfx->drawLine(cx - 3, cy + 10, cx + 14, cy - 10, color);
}

static void drawSelectButton(int cx, int cy) {
  // 详情页使用明确的“选择”按钮，触摸命中区会比按钮本身更大。
  gfx->fillRoundRect(cx - 62, cy - 21, 124, 42, 9, C565(0x55, 0xb3, 0x83));
  gfx->drawRoundRect(cx - 62, cy - 21, 124, 42, 9, UI_INK);
  gfx->setTextColor(UI_WHITE);
  gfx->setTextSize(1);
  gfx->setCursor(cx - uiTextWidth("选择", 6) / 2, cy - 11);
  gfx->print("选择");
}

static void drawCancelIcon(int cx, int cy, uint16_t color) {
  for (int k = -1; k <= 1; k++) {
    gfx->drawLine(cx - 13, cy - 13 + k, cx + 13, cy + 13 + k, color);
    gfx->drawLine(cx + 13, cy - 13 + k, cx - 13, cy + 13 + k, color);
  }
}

static void drawConfirmIcon(int cx, int cy, uint16_t color) {
  for (int k = -1; k <= 1; k++) {
    gfx->drawLine(cx - 15, cy + k, cx - 5, cy + 10 + k, color);
    gfx->drawLine(cx - 5, cy + 10 + k, cx + 16, cy - 13 + k, color);
  }
}

void renderGameMenu() {
  drawGameScene();
  uint16_t panel = C565(0x18, 0x22, 0x38);
  gfx->fillCircle(CX, CY, 231, panel);
  gfx->drawCircle(CX, CY, 231, UI_WHITE);

  gfx->setTextColor(UI_WHITE);
  gfx->setTextSize(3);
  gfx->setCursor(CX - uiTextWidth(T(S_GAME_MENU), 9) / 2, 22);
  gfx->print(T(S_GAME_MENU));

  gfx->fillRoundRect(123, 84, 220, 52, 13, C565(0xc8, 0x58, 0x4f));
  gfx->fillRoundRect(123, 146, 220, 52, 13, C565(0x34, 0x98, 0xdb));
  gfx->fillRoundRect(123, 208, 220, 52, 13, C565(0x2e, 0xcc, 0x71));
  gfx->fillRoundRect(123, 270, 220, 52, 13, C565(0xc0, 0x7a, 0x24));
  gfx->setTextColor(UI_WHITE);
  gfx->setTextSize(gLang == LANG_ZH ? 1 : 2);
  int cell = gLang == LANG_ZH ? 6 : 12;
  int lh = gLang == LANG_ZH ? 25 : 16;
  gfx->setCursor(CX - uiTextWidth(T(S_BATTLE_WILD), cell) / 2, 84 + (52 - lh) / 2);
  gfx->print(T(S_BATTLE_WILD));
  gfx->setCursor(CX - uiTextWidth(T(S_GAME_BALL), cell) / 2, 146 + (52 - lh) / 2);
  gfx->print(T(S_GAME_BALL));
  gfx->setCursor(CX - uiTextWidth(T(S_GAME_MEMORY), cell) / 2, 208 + (52 - lh) / 2);
  gfx->print(T(S_GAME_MEMORY));
  gfx->setCursor(CX - uiTextWidth(T(S_TRAIN_STR), cell) / 2, 270 + (52 - lh) / 2);
  gfx->print(T(S_TRAIN_STR));
  gfx->setTextColor(C565(0xff,0xd0,0x43));
  gfx->setTextSize(1);
  gfx->setCursor(112, 338);
  gfx->print("每日任务  喂食 清洁 游戏");
  char task[12]; snprintf(task, sizeof(task), "%u/3", (unsigned)pet.dailyProgress());
  gfx->setCursor(338, 338); gfx->print(task);
  gfx->flush();
}

void renderMemoryGame() {
  // Touches used to redraw the whole circular scene twice in one loop. Keep a
  // static frame until a timer or input actually changes the memory board.
  static bool cacheValid = false;
  static uint32_t cacheSession = 0;
  static bool cacheNight = false;
  static uint8_t cacheLang = 0;
  static MemoryPhase cachePhase = MEM_RESULT;
  static uint8_t cacheRound = 0, cacheStep = 0, cacheInput = 0;
  static uint16_t cacheScore = 0;
  static int8_t cacheFlash = -2;
  stepMemoryGame();
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  if (cacheValid && cacheSession == memorySession && cacheNight == night &&
      cacheLang == (uint8_t)gLang && cachePhase == memoryPhase &&
      cacheRound == memoryRound && cacheStep == memoryStep &&
      cacheInput == memoryInput && cacheScore == memoryScore &&
      cacheFlash == memoryFlash) {
    return;
  }
  drawGameScene();
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;

  gfx->setTextColor(ink);
  gfx->setTextSize(2);
  gfx->setCursor(CX - uiTextWidth(T(S_GAME_MEMORY), 6) / 2, 28);
  gfx->print(T(S_GAME_MEMORY));

  char score[24];
  snprintf(score, sizeof(score), T(S_SCORE_FMT), (unsigned)memoryScore);
  gfx->setTextSize(2);
  gfx->setCursor(86, 62);
  gfx->print(score);
  char rec[24];
  snprintf(rec, sizeof(rec), T(S_RECORD_FMT), (unsigned)pet.memoryHi);
  gfx->setCursor(378 - uiTextWidth(rec, 6), 62);
  gfx->print(rec);

  for (int i = 0; i < 16; i++) {
    int row = i / 4, col = i % 4;
    int x = MEM_GRID_X + col * MEM_GRID_STEP;
    int y = MEM_GRID_Y + row * MEM_GRID_STEP;
    bool lit = (memoryFlash == i);
    gfx->fillRoundRect(x, y, MEM_CELL, MEM_CELL, 10, lit ? UI_WHITE : MEMORY_COLORS[i]);
    gfx->drawRoundRect(x, y, MEM_CELL, MEM_CELL, 10, lit ? UI_WHITE : ink);
  }

  if (memoryPhase == MEM_RESULT) {
    gfx->fillRoundRect(78, 138, 310, 178, 18, UI_WHITE);
    gfx->drawRoundRect(78, 138, 310, 178, 18, ink);
    const char *msg = memoryWon ? T(S_MEM_WIN) : T(S_MEM_FAIL);
    gfx->setTextColor(memoryWon ? UI_BAR_OK : UI_BAR_BAD);
    gfx->setTextSize(3);
    gfx->setCursor(CX - uiTextWidth(msg, 9) / 2, 164);
    gfx->print(msg);
    gfx->setTextColor(ink);
    gfx->setTextSize(2);
    gfx->setCursor(CX - uiTextWidth(score, 6) / 2, 218);
    gfx->print(score);
    gfx->setCursor(CX - uiTextWidth(rec, 6) / 2, 256);
    gfx->print(rec);
  } else {
    const char *hint = memoryPhase == MEM_SHOWING ? T(S_MEM_WATCH) : T(S_MEM_TURN);
    gfx->setTextColor(ink);
    gfx->setTextSize(2);
    gfx->setCursor(CX - uiTextWidth(hint, 6) / 2, 374);
    gfx->print(hint);
    char round[12];
    snprintf(round, sizeof(round), "%u/30", memoryRound);
    gfx->setTextSize(2);
    gfx->setCursor(CX - uiTextWidth(round, 6) / 2, 398);
    gfx->print(round);
  }
  gfx->flush();
  cacheValid = true;
  cacheSession = memorySession;
  cacheNight = night;
  cacheLang = (uint8_t)gLang;
  cachePhase = memoryPhase;
  cacheRound = memoryRound;
  cacheStep = memoryStep;
  cacheInput = memoryInput;
  cacheScore = memoryScore;
  cacheFlash = memoryFlash;
}

static void drawTopCoins(int right, int y, uint16_t color) {
  // 主页/商店专用的紧凑金币显示，预留五位数字且不带文字框。
  char coins[8];
  snprintf(coins, sizeof(coins), "%05u", (unsigned)pet.coins);
  gfx->fillCircle(right - 72, y + 10, 9, C565(0xff, 0xd0, 0x43));
  gfx->drawCircle(right - 72, y + 10, 9, color);
  gfx->setTextColor(color);
  gfx->setTextSize(2);
  gfx->setCursor(right - 57, y + 2);
  gfx->print(coins);
}

void drawWorldBadge() {
  drawTopCoins(430, 104, gNight ? UI_WHITE : UI_INK);
}

void openWorldMenu() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony) return;
  worldOpen = true;
  shopOpen = false;
  decorOpen = false;
  warehouseOpen = false;
  shopCategory = -1;
  shopScroll = 0;
  shopFromGallery = false;
  economyMsgUntil = 0;
  sfxPlay(SFX_TAP);
}

static void economyNotice(bool ok) {
  economyMsgOk = ok;
  economyMsgUntil = millis() + 1200;
  sfxPlay(ok ? SFX_TAP : SFX_DENY);
}

static void closeShop() {
  shopOpen = false;
  shopDetailOpen = false;
  shopSession++;
  if (shopFromGallery) {
    shopFromGallery = false;
    galleryOpen = true;
    galleryDetail = 0;
    galleryPmd.unload();
    galleryDirty = true;
  }
}

void worldTap(int16_t x, int16_t y) {
  if (y >= 116 && y <= 218) {
    if (x >= 32 && x < 128) pet.visitRoom(WORLD_HOME);
    else if (x >= 134 && x < 230) pet.visitRoom(WORLD_PARK);
    else if (x >= 236 && x < 332) pet.visitRoom(WORLD_BEACH);
    else if (x >= 338 && x < 434) pet.visitRoom(WORLD_FOREST);
    else return;
    economyMsgUntil = 0;
    sfxPlay(SFX_TAP);
    return;
  }
  if (y >= 322 && y <= 368 && x >= 40 && x <= 132) {
    worldOpen = false;
    shopOpen = true;
    shopCategory = -1;
    shopScroll = 0;
    shopFromGallery = false;
    shopSession++;
    economyMsgUntil = 0;
    sfxPlay(SFX_TAP);
    return;
  }
  if (y >= 322 && y <= 368 && x >= 187 && x <= 279) {
    worldOpen = false;
    decorOpen = true;
    economyMsgUntil = 0;
    sfxPlay(SFX_TAP);
    return;
  }
  if (y >= 322 && y <= 368 && x >= 334 && x <= 426) {
    worldOpen = false;
    warehouseOpen = true;
    sfxPlay(SFX_TAP);
    return;
  }
}

void shopTap(int16_t x, int16_t y) {
  if (shopDetailOpen) {
    // 详情页底部左右翻页，中间回到商品列表。
    if (y >= 388 && y <= 448) {
      if (x >= 150 && x <= 316) {
        shopDetailOpen = false;
        economyMsgUntil = 0;
      }
      return;
    }
    if (y >= 268 && y < 318 && x < 190) {
      if (shopDetailQty > 1) shopDetailQty--;
      return;
    }
    if (y >= 268 && y < 318 && x >= 280) {
      if (shopDetailQty < 9) shopDetailQty++;
      return;
    }
    if (y >= 324 && y < 374) {
      if (x < 220) { shopDetailOpen = false; return; }
      if (x >= 220 && x < 380) {
        const ShopProduct &product = SHOP_PRODUCTS[shopCategory][shopDetailSlot];
        bool ok = (uint32_t)product.price * shopDetailQty <= pet.coins;
        for (uint8_t i = 0; i < shopDetailQty && ok; i++) {
          ok = pet.buyShopProduct((uint8_t)shopCategory, shopDetailSlot);
        }
        economyNotice(ok);
        shopDetailOpen = false;
        return;
      }
      shopDetailOpen = false;
      return;
    }
    return;
  }
  if (shopCategory < 0) {
    // Six categories occupy a centered 2x3 grid, with a generous touch target.
    if (y < 108 || y > 356 || x < 32 || x > 434) return;
    uint8_t col = x < 233 ? 0 : 1;
    uint8_t row = (uint8_t)((y - 108) / 84);
    if (row > 2) return;
    shopCategory = (int8_t)(row * 2 + col);
    shopScroll = 0;
    economyMsgUntil = 0;
    sfxPlay(SFX_TAP);
    return;
  }
  if (y < 68) {
    shopCategory = -1;
    shopScroll = 0;
    economyMsgUntil = 0;
    return;
  }
  if (y >= 64 && y < 344) {
    uint8_t row = (uint8_t)((y - 64) / 56);
    if (row < 5) {
      uint8_t slot = (uint8_t)(shopScroll + row);
      if (slot < SHOP_ITEMS_PER_CATEGORY) {
        shopDetailSlot = slot;
        shopDetailQty = 1;
        shopDetailOpen = true;
        economyMsgUntil = 0;
      }
    }
  }
}

static void drawDecorIcon(uint8_t slot, int cx, int cy, bool night) {
  uint16_t ink = night ? UI_WHITE : UI_INK;
  uint16_t stem = night ? C565(0x78, 0xa0, 0x88) : C565(0x3e, 0x6f, 0x55);
  if (slot == 0) {
    gfx->fillCircle(cx, cy, 26, UI_WHITE);
    gfx->fillCircle(cx, cy, 21, C565(0xe9, 0x4b, 0x45));
    gfx->fillRect(cx - 21, cy - 3, 42, 6, UI_WHITE);
    gfx->fillCircle(cx, cy, 7, UI_WHITE);
    gfx->drawCircle(cx, cy, 26, ink);
  } else if (slot == 1) {
    drawPixelFlower(cx - 18, cy - 14, C565(0xff, 0x8e, 0x9a), stem);
    drawPixelFlower(cx + 18, cy - 8, C565(0x8e, 0xb8, 0xff), stem);
  } else if (slot == 2) {
    gfx->fillTriangle(cx - 38, cy + 24, cx, cy - 34, cx + 38, cy + 24, C565(0xe0, 0x73, 0x59));
    gfx->fillTriangle(cx - 28, cy + 24, cx, cy - 24, cx + 28, cy + 24, C565(0xf4, 0xc1, 0x66));
    gfx->fillRect(cx - 5, cy + 2, 10, 22, C565(0x5b, 0x3d, 0x4e));
  } else if (slot == 3) {
    gfx->fillRect(cx - 3, cy - 4, 6, 52, stem);
    gfx->fillCircle(cx, cy - 16, 24, C565(0xff, 0xe0, 0x8a));
    gfx->fillCircle(cx, cy - 16, 14, C565(0xff, 0xf4, 0xb0));
    gfx->drawCircle(cx, cy - 16, 24, ink);
  } else if (slot == 4) {
    gfx->fillRoundRect(cx - 28, cy - 14, 56, 34, 9, C565(0xd2, 0x68, 0x66));
    gfx->fillRect(cx - 23, cy - 6, 46, 5, C565(0xff, 0xd0, 0x84));
    gfx->fillRect(cx - 5, cy - 14, 10, 34, ink);
    gfx->drawRoundRect(cx - 28, cy - 14, 56, 34, 9, ink);
  } else if (slot == 5) {
    gfx->fillRect(cx - 31, cy - 4, 24, 26, C565(0xe8, 0x68, 0x58));
    gfx->fillRect(cx + 7, cy - 10, 24, 32, C565(0x66, 0x9b, 0xe8));
    gfx->fillRect(cx - 12, cy - 30, 24, 26, C565(0xf0, 0xc3, 0x55));
    gfx->drawRect(cx - 31, cy - 4, 24, 26, ink);
    gfx->drawRect(cx + 7, cy - 10, 24, 32, ink);
    gfx->drawRect(cx - 12, cy - 30, 24, 26, ink);
  } else if (slot == 6) {
    gfx->fillRect(cx - 34, cy - 16, 68, 25, C565(0x4a, 0x9c, 0xd1));
    gfx->fillRect(cx - 22, cy - 31, 25, 15, C565(0xf0, 0x76, 0x5b));
    gfx->fillRect(cx + 7, cy - 31, 23, 15, C565(0xf0, 0xc3, 0x55));
    gfx->fillCircle(cx - 21, cy + 10, 10, ink);
    gfx->fillCircle(cx + 21, cy + 10, 10, ink);
    gfx->fillCircle(cx - 21, cy + 10, 4, UI_WHITE);
    gfx->fillCircle(cx + 21, cy + 10, 4, UI_WHITE);
  } else {
    gfx->fillTriangle(cx, cy - 35, cx - 29, cy, cx, cy + 25, C565(0xff, 0x70, 0x86));
    gfx->fillTriangle(cx, cy - 35, cx + 29, cy, cx, cy + 25, C565(0x76, 0xb8, 0xf0));
    gfx->drawLine(cx, cy - 35, cx, cy + 25, ink);
    gfx->drawLine(cx - 29, cy, cx + 29, cy, ink);
    gfx->drawLine(cx, cy + 25, cx - 12, cy + 40, ink);
  }
}

static void drawLockBadge(int cx, int cy) {
  uint16_t dark = C565(0x18, 0x22, 0x38);
  gfx->fillCircle(cx, cy, 12, C565(0x9b, 0xa0, 0xb0));
  gfx->fillRoundRect(cx - 7, cy - 1, 14, 11, 3, dark);
  gfx->drawRoundRect(cx - 5, cy - 8, 10, 12, 4, dark);
  gfx->fillRect(cx - 1, cy + 2, 2, 5, C565(0xff, 0xd0, 0x43));
}

void decorTap(int16_t x, int16_t y) {
  if (x < 68 || x > 398 || y < 72 || y > 392) return;
  uint8_t col = (uint8_t)((x - 68) / 115);
  uint8_t row = (uint8_t)((y - 72) / 111);
  if (col > 2 || row > 2) return;
  if (x > 68 + col * 115 + 100 || y > 72 + row * 111 + 98) return;
  uint8_t slot = row * 3 + col;
  if (slot >= 8) return;
  if (pet.decorOwned & (1u << slot)) {
    pet.toggleDecor(slot);
    sfxPlay(SFX_TAP);
  } else {
    // El icono bloqueado tambien es una entrada rapida a la tienda.
    decorOpen = false;
    shopOpen = true;
    shopCategory = SHOP_CAT_PROP;
    shopScroll = 0;
    shopFromGallery = false;
    shopSession++;
    economyMsgUntil = 0;
    sfxPlay(SFX_TAP);
  }
}

static void drawWorldTile(int x, int y, int w, int h, const char *label, uint16_t color) {
  gfx->fillRoundRect(x, y, w, h, 14, color);
  gfx->drawRoundRect(x, y, w, h, 14, UI_WHITE);
  gfx->setTextColor(UI_WHITE);
  gfx->setTextSize(gLang == LANG_ZH ? 1 : 2);
  int lh = gLang == LANG_ZH ? 25 : 16;
  int cell = gLang == LANG_ZH ? 6 : 12;
  gfx->setCursor(x + (w - uiTextWidth(label, cell)) / 2, y + (h - lh) / 2);
  gfx->print(label);
}

static void drawWorldSceneTile(uint8_t room, int x, int y, int w, int h, const char *label, uint16_t border) {
  gfx->fillRoundRect(x, y, w, h, 12, C565(0x18, 0x22, 0x38));
  gfx->drawRoundRect(x, y, w, h, 12, border);
  int sx = x + 5, sy = y + 5, sw = w - 10, sh = h - 30;
  uint16_t sky = room == WORLD_BEACH ? C565(0x6e, 0xc7, 0xe8)
                 : room == WORLD_FOREST ? C565(0x54, 0x9a, 0x84)
                 : room == WORLD_PARK ? C565(0x8f, 0xd5, 0xed)
                                      : C565(0xf0, 0xb7, 0x83);
  uint16_t ground = room == WORLD_BEACH ? C565(0xf2, 0xd0, 0x81)
                    : room == WORLD_FOREST ? C565(0x2f, 0x6a, 0x4c)
                    : room == WORLD_PARK ? C565(0x67, 0xb9, 0x6b)
                                         : C565(0xd6, 0xa0, 0x7b);
  gfx->fillRect(sx, sy, sw, sh, sky);
  gfx->fillRect(sx, sy + sh - 16, sw, 16, ground);
  if (room == WORLD_HOME) {
    gfx->fillRect(sx + 15, sy + 13, sw - 30, sh - 20, C565(0xff, 0xe1, 0xa0));
    gfx->fillRect(sx + 21, sy + 19, 18, 13, C565(0x8e, 0xd5, 0xe8));
    gfx->drawRect(sx + 21, sy + 19, 18, 13, C565(0x45, 0x66, 0x78));
    gfx->fillRect(sx + sw - 34, sy + 20, 12, 22, C565(0x8a, 0x57, 0x55));
    gfx->fillTriangle(sx + 10, sy + 14, sx + sw / 2, sy - 2, sx + sw - 10, sy + 14, C565(0xe5, 0x72, 0x5a));
  } else if (room == WORLD_PARK) {
    gfx->fillRect(sx + 18, sy + 13, 5, 25, C565(0x6b, 0x48, 0x35));
    gfx->fillCircle(sx + 20, sy + 10, 14, C565(0x3e, 0x9a, 0x62));
    gfx->fillRect(sx + sw - 26, sy + 24, 4, 16, C565(0x6b, 0x48, 0x35));
    gfx->fillCircle(sx + sw - 24, sy + 20, 10, C565(0x4e, 0xa7, 0x6a));
    gfx->fillRect(sx + 34, sy + sh - 20, sw - 58, 3, C565(0x7f, 0x53, 0x42));
  } else if (room == WORLD_BEACH) {
    gfx->fillRect(sx, sy + sh - 23, sw, 7, C565(0x4f, 0x96, 0xc4));
    gfx->fillRect(sx + 12, sy + sh - 16, sw - 24, 3, C565(0xff, 0xf0, 0xb0));
    gfx->fillRect(sx + 20, sy + 14, 4, sh - 26, C565(0x75, 0x4b, 0x36));
    gfx->fillTriangle(sx + 22, sy + 12, sx + 7, sy + 5, sx + 26, sy + 5, C565(0x42, 0x9b, 0x68));
    gfx->fillTriangle(sx + 23, sy + 13, sx + 37, sy + 7, sx + 26, sy + 3, C565(0x42, 0x9b, 0x68));
  } else {
    gfx->fillTriangle(sx + 20, sy + sh - 12, sx + 9, sy + 12, sx + 31, sy + 12, C565(0x2b, 0x70, 0x51));
    gfx->fillTriangle(sx + sw - 20, sy + sh - 12, sx + sw - 32, sy + 18, sx + sw - 8, sy + 18, C565(0x3b, 0x86, 0x5b));
    gfx->fillRect(sx + 43, sy + sh - 13, 19, 3, C565(0x76, 0x4e, 0x3a));
  }
  gfx->setTextColor(UI_WHITE);
  gfx->setTextSize(gLang == LANG_ZH ? 1 : 2);
  int cell = gLang == LANG_ZH ? 6 : 12;
  int lh = gLang == LANG_ZH ? 25 : 16;
  gfx->setCursor(x + (w - uiTextWidth(label, cell)) / 2, y + h - lh - 2);
  gfx->print(label);
}

void renderDecor() {
  uint32_t now = millis();
  drawScene(roomBiome(), now, gNight);
  if (!pet.isEgg()) drawPet();
  // 标题直接叠在完整场景上，避免矩形黑影遮住圆屏顶部。
  gfx->setTextColor(UI_WHITE);
  gfx->setTextSize(gLang == LANG_ZH ? 1 : 3);
  gfx->setCursor(CX - uiTextWidth(T(S_DECORATE), gLang == LANG_ZH ? 6 : 9) / 2, 18);
  gfx->print(T(S_DECORATE));

  const char *labels[8] = { T(S_BALL), T(S_FLOWERS), T(S_TENT), T(S_LAMP), T(S_DRUM), T(S_BLOCKS), T(S_TRAIN), T(S_KITE) };
  for (uint8_t slot = 0; slot < 8; slot++) {
    int x = 68 + (slot % 3) * 115;
    int y = 72 + (slot / 3) * 111;
    bool owned = (pet.decorOwned & (1u << slot)) != 0;
    bool placed = (pet.roomDecor() & (1u << slot)) != 0;
    uint16_t card = owned ? (placed ? C565(0x3f, 0x8f, 0x6a) : C565(0x35, 0x42, 0x5c))
                          : C565(0x2a, 0x2d, 0x38);
    gfx->fillRoundRect(x, y, 100, 98, 12, card);
    gfx->drawRoundRect(x, y, 100, 98, 12, owned ? UI_WHITE : C565(0x55, 0x5a, 0x68));
    drawDecorIcon(slot, x + 50, y + 40, gNight);
    if (!owned) drawLockBadge(x + 82, y + 16);
    gfx->setTextColor(UI_WHITE);
    gfx->setTextSize(gLang == LANG_ZH ? 1 : 2);
    int lh = gLang == LANG_ZH ? 25 : 16;
    int cell = gLang == LANG_ZH ? 6 : 12;
    gfx->setCursor(x + (100 - uiTextWidth(labels[slot], cell)) / 2, y + 58 + (25 - lh) / 2);
    gfx->print(labels[slot]);
    if (placed) {
      gfx->setTextSize(1);
      gfx->setCursor(x + (100 - uiTextWidth(T(S_PLACED), 6)) / 2, y + 80);
      gfx->print(T(S_PLACED));
    }
  }
  gfx->setTextColor(gNight ? UI_WHITE : UI_INK);
  gfx->setTextSize(gLang == LANG_ZH ? 1 : 2);
  gfx->setCursor(CX - uiTextWidth(T(S_TAP_DECOR), 6) / 2, 400);
  gfx->print(T(S_TAP_DECOR));
  gfx->flush();
}

void renderWorldMenu() {
  uint32_t now = millis();
  drawScene(roomBiome(), now, gNight);
  if (!pet.isEgg()) drawPet();
  // 标题直接叠在完整场景上，避免矩形黑影遮住圆屏顶部。
  gfx->setTextColor(UI_WHITE);
  gfx->setTextSize(gLang == LANG_ZH ? 1 : 3);
  gfx->setCursor(CX - uiTextWidth(T(S_WORLD), gLang == LANG_ZH ? 6 : 9) / 2, 16);
  gfx->print(T(S_WORLD));
  // 四个场景缩略图移到宠物上方，底部只留居中的三个入口。
  drawWorldSceneTile(WORLD_HOME, 32, 116, 96, 88, T(S_HOME), C565(0x4f, 0x93, 0xc4));
  drawWorldSceneTile(WORLD_PARK, 134, 116, 96, 88, T(S_PARK), C565(0x45, 0xa8, 0x65));
  drawWorldSceneTile(WORLD_BEACH, 236, 116, 96, 88, T(S_BEACH), C565(0x2e, 0x9e, 0xc6));
  drawWorldSceneTile(WORLD_FOREST, 338, 116, 96, 88, T(S_FOREST), C565(0x36, 0x76, 0x4e));
  drawWorldTile(40, 322, 92, 38, T(S_SHOP), C565(0xc0, 0x7a, 0x24));
  drawWorldTile(187, 322, 92, 38, T(S_DECORATE), C565(0x8c, 0x62, 0xb7));
  drawWorldTile(334, 322, 92, 38, "仓库", C565(0x3c, 0x78, 0xc2));
  if (economyMsgUntil && millis() < economyMsgUntil) {
    gfx->fillRoundRect(116, 78, 234, 24, 8, economyMsgOk ? UI_BAR_OK : UI_BAR_BAD);
    gfx->setTextColor(UI_WHITE);
    gfx->setTextSize(1);
    const char *msg = economyMsgOk ? T(S_BOUGHT) : T(S_NOT_ENOUGH);
    gfx->setCursor(CX - uiTextWidth(msg, 6) / 2, 82);
    gfx->print(msg);
  }
  gfx->flush();
}

static void drawDecorIcon(uint8_t slot, int cx, int cy, bool night);

static void drawShopIcon(uint8_t item, int cx, int cy) {
  uint16_t ink = gNight ? UI_WHITE : UI_INK;
  if (item <= SHOP_BERRY_GREEN) {
    uint16_t fruit = item == SHOP_BERRY_RED ? C565(0xe8, 0x55, 0x55)
                    : item == SHOP_BERRY_BLUE ? C565(0x53, 0x91, 0xe8)
                    : C565(0x69, 0xb8, 0x66);
    gfx->fillCircle(cx - 12, cy + 1, 18, fruit);
    gfx->fillCircle(cx + 12, cy + 1, 18, fruit);
    gfx->fillCircle(cx, cy - 13, 18, fruit);
    gfx->fillCircle(cx - 5, cy - 17, 4, UI_WHITE);
    gfx->fillTriangle(cx + 1, cy - 30, cx + 22, cy - 38, cx + 13, cy - 18, C565(0x58, 0xa8, 0x67));
    gfx->drawCircle(cx - 12, cy + 1, 18, ink);
    gfx->drawCircle(cx + 12, cy + 1, 18, ink);
    gfx->drawCircle(cx, cy - 13, 18, ink);
  } else if (item == SHOP_CANDY) {
    gfx->fillTriangle(cx - 30, cy - 14, cx - 42, cy - 25, cx - 42, cy - 3, C565(0xff, 0xd0, 0x61));
    gfx->fillTriangle(cx + 30, cy - 14, cx + 42, cy - 25, cx + 42, cy - 3, C565(0xff, 0xd0, 0x61));
    gfx->fillRoundRect(cx - 31, cy - 18, 62, 36, 10, C565(0xf0, 0x70, 0x86));
    gfx->drawRoundRect(cx - 31, cy - 18, 62, 36, 10, ink);
    gfx->fillCircle(cx - 11, cy - 5, 5, C565(0xff, 0xd0, 0x61));
    gfx->fillCircle(cx + 8, cy + 7, 5, C565(0x8f, 0xd5, 0x76));
  } else if (item == SHOP_TOY) {
    drawDecorIcon(0, cx, cy, gNight);
  } else if (item == SHOP_MEDICINE) {
    gfx->fillRoundRect(cx - 18, cy - 13, 36, 42, 7, UI_WHITE);
    gfx->fillRect(cx - 12, cy - 22, 24, 10, C565(0x67, 0xa8, 0xc9));
    gfx->fillRect(cx - 13, cy + 3, 26, 5, C565(0x54, 0xc2, 0x92));
    gfx->fillRect(cx - 4, cy - 5, 8, 20, C565(0xe8, 0x68, 0x58));
    gfx->fillRect(cx - 10, cy + 1, 20, 8, C565(0xe8, 0x68, 0x58));
    gfx->drawRoundRect(cx - 18, cy - 13, 36, 42, 7, ink);
  } else if (item == SHOP_TRAIN_TOKEN) {
    gfx->fillRoundRect(cx - 34, cy - 17, 68, 34, 6, C565(0xf0, 0xc3, 0x55));
    gfx->drawRoundRect(cx - 34, cy - 17, 68, 34, 6, ink);
    gfx->fillCircle(cx - 34, cy, 7, C565(0x18, 0x22, 0x38));
    gfx->fillCircle(cx + 34, cy, 7, C565(0x18, 0x22, 0x38));
    gfx->drawLine(cx - 10, cy - 12, cx - 10, cy + 12, ink);
    gfx->drawLine(cx + 10, cy - 12, cx + 10, cy + 12, ink);
    gfx->fillCircle(cx, cy, 5, C565(0xff, 0x70, 0x86));
  } else {
    gfx->fillRoundRect(cx - 35, cy - 25, 70, 50, 6, C565(0xd0, 0x8a, 0x35));
    gfx->drawRoundRect(cx - 35, cy - 25, 70, 50, 6, ink);
    gfx->drawLine(cx - 35, cy - 9, cx + 35, cy - 9, ink);
    gfx->drawLine(cx, cy - 25, cx, cy + 25, ink);
    gfx->fillCircle(cx - 16, cy + 9, 8, C565(0x53, 0x91, 0xe8));
    gfx->fillCircle(cx + 16, cy + 9, 8, C565(0x69, 0xb8, 0x66));
    gfx->drawCircle(cx - 16, cy + 9, 8, ink);
    gfx->drawCircle(cx + 16, cy + 9, 8, ink);
  }
}

// 每个商品槽位都有独立的卡通化实物图案。列表使用 s=1，详情使用 s=2。
static void drawFoodProduct(uint8_t slot, int x, int y, float u) {
  uint16_t k = UI_INK, red = C565(0xe8,0x65,0x5e), yellow = C565(0xff,0xd0,0x55);
  switch (slot) {
    case 0: gfx->fillCircle(x,y+4*u,18*u,red); gfx->drawCircle(x,y+4*u,18*u,k); gfx->fillRect(x-2*u,y-20*u,4*u,7*u,C565(0x4f,0x8f,0x52)); break; // 苹果
    case 1: gfx->fillRoundRect(x-8*u,y-22*u,16*u,45*u,8*u,yellow); gfx->drawRoundRect(x-8*u,y-22*u,16*u,45*u,8*u,k); gfx->drawLine(x,y-20*u,x+8*u,y-28*u,C565(0x4f,0x8f,0x52)); break; // 香蕉
    case 2: gfx->fillCircle(x,y,20*u,C565(0xf3,0x92,0x35)); gfx->drawCircle(x,y,20*u,k); for(int a=-8;a<=8;a+=8) gfx->drawLine(x+a*u,y-15*u,x+a*u,y+15*u,C565(0xff,0xc5,0x55)); break; // 橙子
    case 3: for(int a=-1;a<=1;a++) for(int b=-1;b<=1;b++) { gfx->fillCircle(x+a*12*u,y+b*11*u,8*u,C565(0x72,0x55,0xb8)); gfx->drawCircle(x+a*12*u,y+b*11*u,8*u,k); } gfx->drawLine(x,y-18*u,x+9*u,y-29*u,C565(0x4f,0x8f,0x52)); break; // 葡萄
    case 4: gfx->fillCircle(x,y+4*u,24*u,C565(0x3e,0xb6,0x68)); gfx->drawCircle(x,y+4*u,24*u,k); gfx->drawLine(x-18*u,y-13*u,x+18*u,y+21*u,C565(0xf5,0xe0,0x8a)); gfx->drawLine(x+18*u,y-13*u,x-18*u,y+21*u,C565(0xf5,0xe0,0x8a)); break; // 西瓜
    case 5: gfx->fillRoundRect(x-32*u,y-10*u,64*u,25*u,5*u,red); gfx->fillRect(x-27*u,y-2*u,54*u,7*u,yellow); gfx->fillRoundRect(x-32*u,y-10*u,64*u,25*u,5*u,k); gfx->fillCircle(x-17*u,y+10*u,5*u,C565(0x71,0x3d,0x2f)); gfx->fillCircle(x+17*u,y+10*u,5*u,C565(0x71,0x3d,0x2f)); break; // 汉堡
    case 6: gfx->fillTriangle(x-33*u,y-4*u,x+33*u,y-4*u,x,y+18*u,C565(0xe5,0xb1,0x6b)); gfx->fillRect(x-29*u,y-12*u,58*u,8*u,C565(0x65,0xb5,0x68)); gfx->drawTriangle(x-33*u,y-4*u,x+33*u,y-4*u,x,y+18*u,k); break; // 三明治
    case 7: gfx->fillRoundRect(x-25*u,y-14*u,50*u,34*u,8*u,C565(0xe5,0xb1,0x6b)); gfx->drawRoundRect(x-25*u,y-14*u,50*u,34*u,8*u,k); gfx->drawLine(x-17*u,y-2*u,x+17*u,y-2*u,C565(0xc2,0x75,0x46)); break; // 面包
    case 8: gfx->fillRoundRect(x-28*u,y-17*u,56*u,36*u,5*u,C565(0xf0,0x76,0x86)); gfx->drawRoundRect(x-28*u,y-17*u,56*u,36*u,5*u,k); gfx->fillCircle(x-13*u,y-3*u,5*u,yellow); gfx->fillCircle(x+11*u,y+5*u,5*u,yellow); gfx->fillRect(x-5*u,y-30*u,10*u,13*u,C565(0xff,0xf1,0xd0)); break; // 蛋糕
    case 9: gfx->fillRoundRect(x-22*u,y-17*u,44*u,35*u,10*u,C565(0xf0,0xc9,0x77)); gfx->drawRoundRect(x-22*u,y-17*u,44*u,35*u,10*u,k); gfx->fillCircle(x,y-5*u,4*u,C565(0xe8,0x65,0x5e)); break; // 布丁
    case 10: gfx->fillCircle(x,y+2*u,20*u,C565(0xe8,0x55,0x65)); gfx->drawCircle(x,y+2*u,20*u,k); gfx->fillTriangle(x-7*u,y-14*u,x+4*u,y-29*u,x+13*u,y-12*u,C565(0x4f,0x8f,0x52)); break; // 草莓
    case 11: gfx->fillCircle(x,y+2*u,20*u,C565(0xf2,0x9d,0x75)); gfx->drawCircle(x,y+2*u,20*u,k); gfx->fillTriangle(x-8*u,y-14*u,x,y-25*u,x+8*u,y-14*u,C565(0x4f,0x8f,0x52)); break; // 桃子
    case 12: gfx->fillCircle(x-9*u,y+3*u,13*u,red); gfx->fillCircle(x+9*u,y+3*u,13*u,red); gfx->drawCircle(x-9*u,y+3*u,13*u,k); gfx->drawCircle(x+9*u,y+3*u,13*u,k); gfx->drawLine(x,y-8*u,x+7*u,y-19*u,C565(0x4f,0x8f,0x52)); break; // 樱桃
    case 13: gfx->fillEllipse(x,y+2*u,20*u,27*u,C565(0xf0,0xc3,0x55)); gfx->drawEllipse(x,y+2*u,20*u,27*u,k); gfx->fillTriangle(x-14*u,y-22*u,x+4*u,y-42*u,x+17*u,y-20*u,C565(0x4f,0x8f,0x52)); break; // 菠萝
    case 14: gfx->fillCircle(x,y+5*u,22*u,C565(0x8c,0x55,0x38)); gfx->drawCircle(x,y+5*u,22*u,k); gfx->fillCircle(x,y+5*u,13*u,C565(0xf5,0xe0,0x9a)); gfx->fillTriangle(x-9*u,y-16*u,x,y-31*u,x+10*u,y-16*u,C565(0x4f,0x8f,0x52)); break; // 椰子
    case 15: gfx->fillRoundRect(x-25*u,y-17*u,50*u,34*u,7*u,C565(0xe8,0xc5,0x80)); gfx->drawRoundRect(x-25*u,y-17*u,50*u,34*u,7*u,k); for(int a=-12;a<=12;a+=12) gfx->fillCircle(x+a*u,y,3*u,red); break; // 饼干
    case 16: gfx->fillCircle(x,y,24*u,C565(0xd8,0x75,0x52)); gfx->drawCircle(x,y,24*u,k); gfx->fillCircle(x,y,10*u,UI_WHITE); gfx->fillCircle(x-8*u,y-8*u,3*u,yellow); break; // 甜甜圈
    case 17: gfx->fillRoundRect(x-13*u,y-22*u,26*u,42*u,8*u,C565(0xe5,0x8b,0xb6)); gfx->drawRoundRect(x-13*u,y-22*u,26*u,42*u,8*u,k); gfx->fillCircle(x,y-26*u,11*u,C565(0xf7,0xd6,0xe9)); break; // 冰淇淋
    case 18: gfx->fillRoundRect(x-20*u,y-17*u,40*u,35*u,8*u,C565(0xff,0xf0,0xc4)); gfx->drawRoundRect(x-20*u,y-17*u,40*u,35*u,8*u,k); gfx->fillCircle(x,y-5*u,6*u,C565(0xf0,0x9c,0x56)); gfx->fillRect(x-2*u,y-28*u,4*u,12*u,C565(0xff,0x9b,0x4a)); break; // 热牛奶
    default: gfx->fillRoundRect(x-28*u,y-14*u,56*u,28*u,6*u,C565(0x65,0xb5,0x68)); gfx->drawRoundRect(x-28*u,y-14*u,56*u,28*u,6*u,k); gfx->fillTriangle(x+20*u,y-8*u,x+34*u,y,x+20*u,y+8*u,red); break; // 能量棒
  }
}

static void drawToyProduct(uint8_t slot, int x, int y, float u) {
  uint16_t k=UI_INK, blue=C565(0x5b,0x9e,0xe8), pink=C565(0xe8,0x75,0x86), yellow=C565(0xff,0xd0,0x55), red=C565(0xe8,0x65,0x5e);
  switch(slot) {
    case 0: gfx->fillCircle(x,y,22*u,blue); gfx->drawCircle(x,y,22*u,k); gfx->drawLine(x-20*u,y,x+20*u,y,UI_WHITE); break;
    case 1: gfx->fillCircle(x-12*u,y-5*u,12*u,pink); gfx->fillCircle(x+12*u,y-5*u,12*u,pink); gfx->fillRoundRect(x-22*u,y-3*u,44*u,28*u,10*u,pink); gfx->drawRoundRect(x-22*u,y-3*u,44*u,28*u,10*u,k); gfx->fillCircle(x-8*u,y+6*u,3*u,k); gfx->fillCircle(x+8*u,y+6*u,3*u,k); break;
    case 2: gfx->drawLine(x-24*u,y-7*u,x+24*u,y+7*u,k); gfx->fillCircle(x-28*u,y-9*u,8*u,red); gfx->fillCircle(x+28*u,y+9*u,8*u,blue); break;
    case 3: gfx->fillCircle(x,y,18*u,yellow); gfx->drawCircle(x,y,18*u,k); gfx->drawLine(x,y-18*u,x,y-30*u,k); gfx->drawLine(x+13*u,y-12*u,x+24*u,y-22*u,k); break;
    case 4: for(int a=-1;a<=1;a++) for(int b=-1;b<=1;b++){gfx->fillRect(x+a*17*u,y+b*15*u,15*u,14*u,(a+b+3)%2?blue:yellow);gfx->drawRect(x+a*17*u,y+b*15*u,15*u,14*u,k);} break;
    case 5: gfx->fillTriangle(x,y-26*u,x-28*u,y+15*u,x+28*u,y+15*u,pink); gfx->drawLine(x,y-26*u,x,y+21*u,k); gfx->drawLine(x-28*u,y+15*u,x+28*u,y+15*u,k); break;
    case 6: gfx->fillRoundRect(x-25*u,y-15*u,50*u,30*u,6*u,C565(0x4a,0x9c,0xd1)); gfx->drawRoundRect(x-25*u,y-15*u,50*u,30*u,6*u,k); gfx->fillCircle(x-17*u,y+22*u,8*u,k); gfx->fillCircle(x+17*u,y+22*u,8*u,k); break;
    case 7: gfx->fillRect(x-30*u,y-12*u,60*u,25*u,blue); gfx->drawRect(x-30*u,y-12*u,60*u,25*u,k); gfx->fillRect(x-19*u,y-29*u,18*u,17*u,yellow); gfx->fillRect(x+4*u,y-29*u,18*u,17*u,pink); gfx->fillCircle(x-20*u,y+18*u,8*u,k); gfx->fillCircle(x+20*u,y+18*u,8*u,k); break;
    case 8: gfx->fillRoundRect(x-20*u,y-14*u,40*u,30*u,6*u,C565(0x9d,0xd9,0xf0)); gfx->drawRoundRect(x-20*u,y-14*u,40*u,30*u,6*u,k); gfx->fillCircle(x+27*u,y-20*u,6*u,blue); gfx->fillCircle(x+36*u,y-31*u,4*u,pink); break;
    case 9: gfx->fillRoundRect(x-22*u,y-15*u,44*u,30*u,5*u,k); gfx->fillRect(x-12*u,y-7*u,24*u,14*u,C565(0x71,0xe0,0x75)); gfx->drawLine(x+22*u,y,x+39*u,y-12*u,C565(0x71,0xe0,0x75)); break;
    case 10: gfx->fillEllipse(x,y,28*u,12*u,blue); gfx->drawEllipse(x,y,28*u,12*u,k); gfx->drawLine(x-8*u,y,x+8*u,y,k); break;
    case 11: gfx->drawCircle(x-17*u,y,18*u,pink); gfx->drawCircle(x+17*u,y,18*u,pink); gfx->drawLine(x-17*u,y-18*u,x+17*u,y+18*u,k); gfx->drawLine(x+17*u,y-18*u,x-17*u,y+18*u,k); break;
    case 12: gfx->fillTriangle(x-27*u,y+15*u,x+27*u,y+15*u,x-5*u,y-22*u,UI_WHITE); gfx->drawTriangle(x-27*u,y+15*u,x+27*u,y+15*u,x-5*u,y-22*u,k); gfx->drawLine(x-5*u,y-22*u,x+12*u,y+15*u,k); break;
    case 13: gfx->fillRoundRect(x-22*u,y-22*u,44*u,44*u,5*u,blue); gfx->drawRoundRect(x-22*u,y-22*u,44*u,44*u,5*u,k); gfx->drawLine(x-22*u,y,x+22*u,y,k); gfx->drawLine(x,y-22*u,x,y+22*u,k); break;
    case 14: gfx->fillRoundRect(x-24*u,y-17*u,48*u,34*u,4*u,C565(0xd6,0x8a,0x49)); gfx->drawRoundRect(x-24*u,y-17*u,48*u,34*u,4*u,k); gfx->fillCircle(x,y,7*u,yellow); break;
    case 15: gfx->fillRoundRect(x-24*u,y-10*u,48*u,20*u,6*u,blue); gfx->drawRoundRect(x-24*u,y-10*u,48*u,20*u,6*u,k); gfx->fillCircle(x+26*u,y-3*u,6*u,blue); gfx->drawLine(x+29*u,y-2*u,x+42*u,y-13*u,blue); break;
    case 16: gfx->fillRoundRect(x-25*u,y-19*u,50*u,35*u,4*u,yellow); gfx->drawRoundRect(x-25*u,y-19*u,50*u,35*u,4*u,k); gfx->drawLine(x-16*u,y-19*u,x-16*u,y-32*u,k); gfx->drawLine(x+16*u,y-19*u,x+16*u,y-32*u,k); break;
    case 17: gfx->drawCircle(x,y,21*u,k); gfx->drawCircle(x,y,14*u,blue); gfx->drawLine(x-30*u,y+22*u,x+30*u,y-22*u,k); break;
    case 18: gfx->fillRoundRect(x-19*u,y-18*u,38*u,35*u,5*u,C565(0x71,0xc4,0x8e)); gfx->drawRoundRect(x-19*u,y-18*u,38*u,35*u,5*u,k); gfx->fillCircle(x-8*u,y-5*u,4*u,k); gfx->fillCircle(x+8*u,y-5*u,4*u,k); gfx->drawLine(x-24*u,y-27*u,x-12*u,y-18*u,k); gfx->drawLine(x+24*u,y-27*u,x+12*u,y-18*u,k); break;
    default: gfx->fillCircle(x,y,20*u,C565(0xd6,0x8a,0xe5)); gfx->drawCircle(x,y,20*u,k); for(int a=0;a<8;a++){int dx=(a%2?24:-24)*u,dy=(a/2-1)*10*u; gfx->fillCircle(x+dx,y+dy,3*u,yellow);} break;
  }
}

static void drawMedicineProduct(uint8_t slot, int x, int y, float u) {
  uint16_t k=UI_INK, red=C565(0xe8,0x65,0x5e), green=C565(0x5f,0xc4,0x96), blue=C565(0x65,0xa8,0xc7);
  if (slot == 1) { gfx->fillRoundRect(x-30*u,y-8*u,60*u,16*u,5*u,UI_WHITE); gfx->drawRoundRect(x-30*u,y-8*u,60*u,16*u,5*u,k); gfx->drawLine(x-20*u,y-5*u,x+20*u,y+5*u,red); return; }
  if (slot == 6) { gfx->fillCircle(x,y,22*u,C565(0x5f,0x6f,0xc8)); gfx->drawCircle(x,y,22*u,k); gfx->fillCircle(x+8*u,y-9*u,10*u,C565(0xff,0xf0,0xb0)); return; }
  if (slot == 8 || slot == 13) { gfx->fillRoundRect(x-17*u,y-24*u,34*u,45*u,6*u,blue); gfx->drawRoundRect(x-17*u,y-24*u,34*u,45*u,6*u,k); gfx->fillRect(x-4*u,y-37*u,8*u,16*u,C565(0x9d,0xd9,0xf0)); gfx->drawLine(x-11*u,y-4*u,x+11*u,y+7*u,green); return; }
  if (slot == 15) { gfx->fillRoundRect(x-22*u,y-17*u,44*u,34*u,8*u,C565(0xb6,0xe3,0xff)); gfx->drawRoundRect(x-22*u,y-17*u,44*u,34*u,8*u,k); gfx->fillCircle(x,y,8*u,UI_WHITE); return; }
  if (slot == 17) { gfx->fillRoundRect(x-8*u,y-29*u,16*u,51*u,5*u,red); gfx->drawRoundRect(x-8*u,y-29*u,16*u,51*u,5*u,k); gfx->fillCircle(x,y-38*u,9*u,red); gfx->drawLine(x,y-17*u,x,y+12*u,UI_WHITE); return; }
  if (slot == 18) { gfx->fillCircle(x,y,22*u,C565(0x73,0xc8,0x75)); gfx->drawCircle(x,y,22*u,k); gfx->drawLine(x,y-16*u,x-5*u,y+15*u,k); return; }
  gfx->fillRoundRect(x-19*u,y-24*u,38*u,48*u,7*u,UI_WHITE); gfx->drawRoundRect(x-19*u,y-24*u,38*u,48*u,7*u,k); gfx->fillRect(x-11*u,y-32*u,22*u,9*u,blue);
  uint16_t c = (slot % 3 == 0) ? red : (slot % 3 == 1 ? green : C565(0xff,0xc8,0x5c));
  gfx->fillRect(x-4*u,y-13*u,8*u,25*u,c); gfx->fillRect(x-12*u,y-4*u,24*u,8*u,c);
}

static void drawEquipmentProduct(uint8_t slot, int x, int y, float u) {
  uint16_t k=UI_INK, steel=C565(0xd6,0xd9,0xe3), blue=C565(0x5a,0x92,0xd7), gold=C565(0xff,0xd0,0x55);
  if (slot == 0 || slot == 10) { gfx->fillRect(x-4*u,y-34*u,8*u,55*u,steel); gfx->fillTriangle(x-15*u,y-34*u,x+15*u,y-34*u,x,y-48*u,gold); gfx->drawLine(x,y-34*u,x,y+21*u,k); return; }
  if (slot == 1 || slot == 4 || slot == 7 || slot == 16) { gfx->fillCircle(x,y,26*u,blue); gfx->drawCircle(x,y,26*u,k); gfx->drawLine(x-18*u,y,x+18*u,y,k); gfx->fillCircle(x,y,8*u,gold); return; }
  if (slot == 2) { gfx->fillRoundRect(x-27*u,y-18*u,54*u,35*u,12*u,steel); gfx->drawRoundRect(x-27*u,y-18*u,54*u,35*u,12*u,k); gfx->fillRect(x-7*u,y-27*u,14*u,9*u,blue); return; }
  if (slot == 3 || slot == 8 || slot == 15) { gfx->fillRoundRect(x-28*u,y-21*u,56*u,43*u,7*u,C565(0xa9,0x55,0x4d)); gfx->drawRoundRect(x-28*u,y-21*u,56*u,43*u,7*u,k); gfx->drawLine(x,y-18*u,x,y+18*u,gold); return; }
  if (slot == 5) { gfx->fillTriangle(x,y-32*u,x-18*u,y+27*u,x+18*u,y+27*u,blue); gfx->drawTriangle(x,y-32*u,x-18*u,y+27*u,x+18*u,y+27*u,k); return; }
  if (slot == 6) { gfx->fillRoundRect(x-25*u,y-14*u,50*u,28*u,7*u,C565(0xd5,0x87,0x59)); gfx->drawRoundRect(x-25*u,y-14*u,50*u,28*u,7*u,k); gfx->fillCircle(x-18*u,y+20*u,8*u,k); gfx->fillCircle(x+18*u,y+20*u,8*u,k); return; }
  if (slot == 9 || slot == 12 || slot == 13 || slot == 14) { gfx->fillCircle(x,y,24*u,slot==12?C565(0xf0,0x6d,0x5c):slot==13?C565(0x72,0xb6,0xed):blue); gfx->drawCircle(x,y,24*u,k); gfx->drawLine(x-14*u,y-14*u,x+14*u,y+14*u,gold); return; }
  if (slot == 11) { gfx->fillRoundRect(x-24*u,y-22*u,48*u,44*u,5*u,steel); gfx->drawRoundRect(x-24*u,y-22*u,48*u,44*u,5*u,k); gfx->fillRect(x-4*u,y-31*u,8*u,9*u,gold); return; }
  if (slot == 17) { gfx->fillRoundRect(x-25*u,y-19*u,50*u,38*u,6*u,C565(0x3c,0x3c,0x50)); gfx->drawRoundRect(x-25*u,y-19*u,50*u,38*u,6*u,k); gfx->fillCircle(x,y,13*u,C565(0x47,0x89,0x7d)); return; }
  if (slot == 18) { gfx->fillCircle(x,y,24*u,C565(0x54,0xd0,0xd0)); gfx->drawCircle(x,y,24*u,k); gfx->fillCircle(x,y,12*u,C565(0x9d,0xf1,0xf1)); return; }
  gfx->fillRoundRect(x-25*u,y-25*u,50*u,45*u,5*u,gold); gfx->drawRoundRect(x-25*u,y-25*u,50*u,45*u,5*u,k); gfx->fillTriangle(x-18*u,y-25*u,x+18*u,y-25*u,x,y-42*u,C565(0xf0,0x75,0x5d));
}

static void drawTravelProduct(uint8_t slot, int x, int y, float u) {
  uint16_t k=UI_INK, sky=C565(0x6e,0xc7,0xe8), stone=C565(0xe3,0xb3,0x72), green=C565(0x55,0xa7,0x63), gold=C565(0xff,0xd0,0x55), red=C565(0xe8,0x65,0x5e), white=C565(0xf1,0xf1,0xe6);
  gfx->fillRect(x-34*u,y-26*u,68*u,54*u,sky); gfx->fillRect(x-34*u,y+10*u,68*u,18*u,green); gfx->drawRect(x-34*u,y-26*u,68*u,54*u,k);
  switch(slot) {
    case 0: gfx->fillTriangle(x,y-22*u,x-27*u,y+17*u,x+27*u,y+17*u,stone); gfx->drawLine(x,y-22*u,x,y+17*u,k); break; // 长城/金字塔
    case 1: gfx->fillTriangle(x,y-23*u,x-21*u,y+17*u,x+21*u,y+17*u,stone); gfx->fillRect(x-4*u,y+3*u,8*u,14*u,k); break;
    case 2: gfx->fillRect(x-22*u,y-22*u,44*u,39*u,stone); gfx->fillTriangle(x-27*u,y-22*u,x,y-39*u,x+27*u,y-22*u,C565(0xc9,0x62,0x4f)); break;
    case 3: gfx->fillRect(x-22*u,y-18*u,44*u,35*u,stone); gfx->drawCircle(x-11*u,y-1*u,9*u,k); gfx->drawCircle(x+11*u,y-1*u,9*u,k); break;
    case 4: gfx->fillRect(x-20*u,y-18*u,40*u,36*u,stone); gfx->fillTriangle(x-26*u,y-18*u,x,y-35*u,x+26*u,y-18*u,C565(0xe9,0x7f,0x60)); break;
    case 5: gfx->fillRoundRect(x-28*u,y-12*u,56*u,22*u,10*u,C565(0xf1,0xf1,0xf1)); gfx->drawRoundRect(x-28*u,y-12*u,56*u,22*u,10*u,k); for(int a=-18;a<=18;a+=12) gfx->drawLine(x+a*u,y-7*u,x+a*u,y+6*u,C565(0xc7,0x58,0x68)); break;
    case 6: gfx->fillRect(x-18*u,y-20*u,36*u,38*u,stone); gfx->fillTriangle(x-25*u,y-20*u,x,y-35*u,x+25*u,y-20*u,gold); break;
    case 7: gfx->fillRoundRect(x-27*u,y-20*u,54*u,39*u,8*u,C565(0x55,0x92,0xc5)); gfx->drawRoundRect(x-27*u,y-20*u,54*u,39*u,8*u,k); gfx->drawLine(x-17*u,y+8*u,x+17*u,y-8*u,C565(0xff,0xff,0xff)); break;
    case 8: gfx->fillTriangle(x,y-26*u,x-26*u,y+18*u,x+26*u,y+18*u,stone); gfx->drawTriangle(x,y-26*u,x-26*u,y+18*u,x+26*u,y+18*u,k); gfx->fillRect(x-6*u,y+3*u,12*u,15*u,C565(0x6f,0x4a,0x36)); break;
    case 10: gfx->fillRect(x-6*u,y-23*u,12*u,41*u,gold); gfx->fillCircle(x,y-28*u,9*u,gold); gfx->fillCircle(x-14*u,y-12*u,7*u,gold); gfx->fillCircle(x+14*u,y-12*u,7*u,gold); gfx->drawLine(x,y-18*u,x-23*u,y-3*u,k); gfx->drawLine(x,y-18*u,x+23*u,y-3*u,k); break;
    case 11: gfx->fillRect(x-13*u,y-16*u,26*u,35*u,white); gfx->fillTriangle(x-17*u,y-16*u,x+17*u,y-16*u,x,y-27*u,red); gfx->fillRect(x-26*u,y+17*u,52*u,5*u,stone); gfx->fillRect(x-5*u,y-8*u,10*u,9*u,sky); break;
    case 12: gfx->drawLine(x,y-31*u,x-24*u,y+18*u,k); gfx->drawLine(x,y-31*u,x+24*u,y+18*u,k); gfx->drawLine(x-12*u,y-6*u,x+12*u,y-6*u,k); gfx->drawLine(x-18*u,y+8*u,x+18*u,y+8*u,k); gfx->drawLine(x-6*u,y-30*u,x-6*u,y+17*u,k); gfx->drawLine(x+6*u,y-30*u,x+6*u,y+17*u,k); break;
    case 13: gfx->fillRect(x-8*u,y-25*u,16*u,43*u,stone); gfx->fillRect(x-24*u,y+13*u,48*u,6*u,stone); gfx->drawLine(x-8*u,y-25*u,x+12*u,y-25*u,k); gfx->drawCircle(x,y-12*u,6*u,k); gfx->drawCircle(x+1*u,y+2*u,6*u,k); break;
    case 14: gfx->fillRoundRect(x-31*u,y-17*u,62*u,35*u,8*u,stone); gfx->drawRoundRect(x-31*u,y-17*u,62*u,35*u,8*u,k); for(int a=-2;a<=2;a++){gfx->fillCircle(x+a*12*u,y-2*u,6*u,k);gfx->fillRect(x+a*12*u-6*u,y-2*u,12*u,19*u,stone);} break;
    case 15: gfx->fillRoundRect(x-18*u,y-23*u,36*u,40*u,4*u,stone); gfx->drawRoundRect(x-18*u,y-23*u,36*u,40*u,4*u,k); gfx->fillTriangle(x-25*u,y-23*u,x,y-34*u,x+25*u,y-23*u,stone); gfx->fillRect(x-4*u,y-1*u,8*u,18*u,k); break;
    case 16: gfx->fillTriangle(x-27*u,y+15*u,x-12*u,y-23*u,x+1*u,y+15*u,white); gfx->fillTriangle(x-10*u,y+15*u,x+7*u,y-28*u,x+23*u,y+15*u,white); gfx->fillTriangle(x+7*u,y+15*u,x+23*u,y-18*u,x+33*u,y+15*u,white); gfx->drawLine(x-29*u,y+16*u,x+31*u,y+16*u,k); break;
    case 17: gfx->fillRect(x-20*u,y-4*u,40*u,22*u,white); gfx->fillCircle(x,y-7*u,16*u,white); gfx->fillTriangle(x-18*u,y-6*u,x,y-24*u,x+18*u,y-6*u,white); gfx->fillRect(x-26*u,y-13*u,5*u,31*u,white); gfx->fillRect(x+21*u,y-13*u,5*u,31*u,white); gfx->drawCircle(x,y-7*u,16*u,k); break;
    case 18: for(int a=-1;a<=1;a+=2){gfx->fillRect(x+a*13*u,y-21*u,9*u,40*u,white);gfx->drawRect(x+a*13*u,y-21*u,9*u,40*u,k);gfx->drawLine(x+a*13*u,y-14*u,x+a*13*u+8*u,y-14*u,k);gfx->drawLine(x+a*13*u,y-4*u,x+a*13*u+8*u,y-4*u,k);} gfx->drawLine(x-18*u,y+9*u,x+18*u,y+9*u,k); break;
    default: gfx->fillTriangle(x,y-31*u,x-31*u,y+18*u,x+31*u,y+18*u,C565(0x88,0xa8,0xd8)); gfx->drawTriangle(x,y-31*u,x-31*u,y+18*u,x+31*u,y+18*u,k); gfx->fillTriangle(x,y-21*u,x-21*u,y+18*u,x+21*u,y+18*u,white); break;
  }
}

static void drawPropProduct(uint8_t slot, int x, int y, float u) {
  uint16_t k=UI_INK, wood=C565(0x9b,0x68,0x47), green=C565(0x55,0xa7,0x63), blue=C565(0x66,0xb9,0xe8), red=C565(0xe8,0x65,0x5e);
  switch(slot) {
    case 0: gfx->fillRect(x-25*u,y-12*u,50*u,32*u,C565(0xff,0xdf,0x95)); gfx->fillTriangle(x-31*u,y-12*u,x,y-36*u,x+31*u,y-12*u,red); gfx->fillRect(x-6*u,y+3*u,12*u,17*u,wood); break;
    case 1: case 12: gfx->fillRect(x-4*u,y-5*u,8*u,31*u,wood); gfx->fillCircle(x,y-12*u,24*u,slot==12?C565(0xd8,0x8a,0x43):green); break;
    case 2: for(int a=-1;a<=1;a++){gfx->fillCircle(x+a*16*u,y,8*u,a?C565(0xf0,0x80,0xa0):C565(0xff,0xd0,0x55));gfx->drawLine(x+a*16*u,y+7*u,x+a*16*u,y+25*u,green);} break;
    case 3: gfx->fillCircle(x,y-8*u,9*u,blue); gfx->fillRect(x-5*u,y-3*u,10*u,27*u,blue); gfx->fillEllipse(x,y+23*u,29*u,9*u,C565(0x9d,0xd9,0xf0)); break;
    case 4: gfx->fillEllipse(x,y+6*u,30*u,15*u,blue); gfx->drawEllipse(x,y+6*u,30*u,15*u,k); gfx->fillCircle(x+12*u,y+2*u,4*u,green); break;
    case 5: gfx->fillRoundRect(x-29*u,y-10*u,58*u,28*u,8*u,C565(0xd8,0x75,0x72)); gfx->drawRoundRect(x-29*u,y-10*u,58*u,28*u,8*u,k); gfx->drawLine(x,y-10*u,x,y+18*u,k); break;
    case 6: gfx->fillRect(x-30*u,y-10*u,60*u,28*u,C565(0xb6,0xd2,0xf0)); gfx->drawRect(x-30*u,y-10*u,60*u,28*u,k); gfx->fillRect(x-25*u,y-7*u,20*u,10*u,UI_WHITE); break;
    case 7: gfx->fillEllipse(x,y-8*u,30*u,11*u,C565(0xd6,0x9b,0x5d)); gfx->fillRect(x-22*u,y,5*u,25*u,wood); gfx->fillRect(x+17*u,y,5*u,25*u,wood); break;
    case 8: gfx->fillCircle(x,y-15*u,18*u,C565(0xff,0xe0,0x8a)); gfx->drawCircle(x,y-15*u,18*u,k); gfx->fillRect(x-3*u,y+3*u,6*u,24*u,wood); break;
    case 9: gfx->fillRoundRect(x-30*u,y-14*u,60*u,29*u,5*u,C565(0x8c,0x62,0xb7)); gfx->drawRoundRect(x-30*u,y-14*u,60*u,29*u,5*u,k); gfx->drawLine(x-22*u,y,x+22*u,y,C565(0xff,0xd0,0x55)); break;
    case 10: gfx->drawLine(x-23*u,y-28*u,x-23*u,y+12*u,k); gfx->drawLine(x+23*u,y-28*u,x+23*u,y+12*u,k); gfx->drawLine(x-23*u,y-28*u,x+23*u,y-28*u,k); gfx->drawLine(x-15*u,y-24*u,x-15*u,y+8*u,k); gfx->drawLine(x+15*u,y-24*u,x+15*u,y+8*u,k); gfx->fillRect(x-20*u,y+8*u,40*u,7*u,red); break;
    case 11: gfx->fillTriangle(x-28*u,y+20*u,x+24*u,y+20*u,x+14*u,y-25*u,blue); gfx->drawTriangle(x-28*u,y+20*u,x+24*u,y+20*u,x+14*u,y-25*u,k); break;
    case 13: gfx->fillCircle(x,y+9*u,20*u,UI_WHITE); gfx->fillCircle(x,y-16*u,15*u,UI_WHITE); gfx->drawCircle(x,y+9*u,20*u,k); gfx->drawCircle(x,y-16*u,15*u,k); gfx->fillCircle(x-5*u,y-19*u,2*u,k); gfx->fillCircle(x+5*u,y-19*u,2*u,k); break;
    case 14: gfx->fillTriangle(x,y-31*u,x-30*u,y+22*u,x+30*u,y+22*u,C565(0xe6,0x79,0x5c)); gfx->drawTriangle(x,y-31*u,x-30*u,y+22*u,x+30*u,y+22*u,k); gfx->fillTriangle(x,y-20*u,x-9*u,y+22*u,x+9*u,y+22*u,C565(0xf2,0xc0,0x6b)); break;
    case 15: gfx->fillTriangle(x,y-20*u,x-14*u,y+17*u,x+14*u,y+17*u,C565(0xff,0x9d,0x36)); gfx->fillTriangle(x,y-11*u,x-8*u,y+16*u,x+8*u,y+16*u,C565(0xff,0xe0,0x55)); gfx->drawLine(x-27*u,y+21*u,x+27*u,y+21*u,wood); break;
    case 16: gfx->fillRect(x-3*u,y-2*u,6*u,30*u,wood); for(int a=0;a<4;a++){int dx=(a%2?24:-24)*u,dy=(a/2?20:-20)*u;gfx->drawLine(x,y-8*u,x+dx,y-8*u+dy,k);} gfx->fillCircle(x,y-8*u,5*u,red); break;
    case 17: gfx->fillRoundRect(x-22*u,y-16*u,44*u,34*u,6*u,red); gfx->drawRoundRect(x-22*u,y-16*u,44*u,34*u,6*u,k); gfx->fillRect(x-3*u,y+18*u,6*u,16*u,wood); gfx->drawLine(x-14*u,y-5*u,x+14*u,y-5*u,UI_WHITE); break;
    case 18: gfx->drawLine(x-31*u,y+17*u,x-18*u,y-5*u,wood); gfx->drawLine(x+31*u,y+17*u,x+18*u,y-5*u,wood); gfx->fillRect(x-23*u,y-5*u,46*u,8*u,C565(0xd6,0x9b,0x5d)); gfx->drawLine(x-18*u,y-5*u,x+18*u,y-5*u,k); break;
    default: for(int a=0;a<4;a++){uint16_t c=a==0?red:a==1?C565(0xff,0xd0,0x55):a==2?green:blue;gfx->drawCircle(x,y+10*u,15*u+a*6*u,c);} gfx->fillRect(x-4*u,y+10*u,8*u,23*u,wood); break;
  }
}

// 商品缩略图：列表和详情都调用同一套实物图案。
static void drawProductIcon(uint8_t category, uint8_t slot, int cx, int cy, uint8_t s) {
  // 列表缩略图放大到可辨认的实物比例，详情页仍使用 2 倍图案。
  float u = s == 1 ? 0.72f : (float)s;
  if (category == SHOP_CAT_FOOD) drawFoodProduct(slot, cx, cy, u);
  else if (category == SHOP_CAT_TOY) drawToyProduct(slot, cx, cy, u);
  else if (category == SHOP_CAT_MEDICINE) drawMedicineProduct(slot, cx, cy, u);
  else if (category == SHOP_CAT_EQUIPMENT) drawEquipmentProduct(slot, cx, cy, u);
  else if (category == SHOP_CAT_PROP) drawPropProduct(slot, cx, cy, u);
  else drawTravelProduct(slot, cx, cy, u);
}

static void drawShopCard(uint8_t item, int x, int y, int w, int h, const char *label, uint16_t color, uint16_t cost) {
  gfx->fillRoundRect(x, y, w, h, 14, color);
  gfx->drawRoundRect(x, y, w, h, 14, UI_WHITE);
  drawShopIcon(item, x + w / 2, y + 30);
  gfx->setTextColor(UI_WHITE);
  gfx->setTextSize(gLang == LANG_ZH ? 1 : 2);
  int cell = gLang == LANG_ZH ? 6 : 12;
  int lh = gLang == LANG_ZH ? 25 : 16;
  gfx->setCursor(x + (w - uiTextWidth(label, cell)) / 2, y + 52 + (25 - lh) / 2);
  gfx->print(label);
  char costText[16];
  snprintf(costText, sizeof(costText), T(S_COST_FMT), (unsigned)cost);
  gfx->setTextSize(1);
  gfx->setCursor(x + (w - uiTextWidth(costText, 6)) / 2, y + h - 18);
  gfx->print(costText);
}

void renderShop() {
  // 商店内容不是动画：只在状态变化时提交一帧，避免连续整屏刷新造成闪屏。
  static bool cacheValid = false;
  static int8_t cacheCategory = -2;
  static bool cacheDetail = false, cacheNight = false, cacheMsgVisible = false, cacheMsgOk = false;
  static uint8_t cacheScroll = 0, cacheSlot = 0, cacheQty = 0, cacheLang = 0;
  static uint32_t cacheSession = 0, cacheCoins = 0;
  bool msgVisible = economyMsgUntil && millis() < economyMsgUntil;
  uint32_t coinsNow = pet.coins;
  if (cacheValid && cacheCategory == shopCategory && cacheDetail == shopDetailOpen &&
      cacheNight == gNight && cacheScroll == shopScroll && cacheSlot == shopDetailSlot &&
      cacheQty == shopDetailQty && cacheLang == (uint8_t)gLang &&
      cacheSession == shopSession && cacheCoins == coinsNow &&
      cacheMsgVisible == msgVisible && cacheMsgOk == economyMsgOk) {
    return;
  }
  gfx->fillScreen(gNight ? C565(0x20,0x2b,0x42) : C565(0x2a,0x3b,0x55));
  gfx->fillCircle(CX, CY, 231, gNight ? C565(0x20,0x2b,0x42) : C565(0x2a,0x3b,0x55));
  gfx->drawCircle(CX, CY, 231, UI_WHITE);
  gfx->setTextColor(UI_WHITE);
  gfx->setTextSize(2);
  const char *title = shopCategory < 0 ? T(S_SHOP) : SHOP_CATEGORY_NAMES[shopCategory];
  gfx->setCursor(CX - uiTextWidth(title, 6) / 2, 16);
  gfx->print(title);
  drawTopCoins(430, 16, UI_WHITE);
  if (shopDetailOpen && shopCategory >= 0) {
    const ShopProduct &p = SHOP_PRODUCTS[shopCategory][shopDetailSlot];
    gfx->fillRoundRect(38, 52, 390, 330, 18, UI_WHITE);
    gfx->drawRoundRect(38, 52, 390, 330, 18, UI_INK);
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(2);
    gfx->setCursor(CX - uiTextWidth(p.name, 12) / 2, 66);
    gfx->print(p.name);
    uint16_t accent = shopCategory == SHOP_CAT_FOOD ? C565(0xe8,0x72,0x57)
                      : shopCategory == SHOP_CAT_TOY ? C565(0x6c,0x9f,0xe8)
                      : shopCategory == SHOP_CAT_MEDICINE ? C565(0x5f,0xc4,0x96)
                      : shopCategory == SHOP_CAT_TRAVEL ? C565(0xd7,0x9a,0x43) : C565(0xa8,0x79,0xd7);
    drawProductIcon((uint8_t)shopCategory, shopDetailSlot, CX, 145, 2);
    gfx->setTextSize(2);
    if (shopCategory == SHOP_CAT_FOOD) {
      char desc[32]; snprintf(desc, sizeof(desc), "饥饿感 +%u", (unsigned)p.value);
      gfx->setCursor(CX - uiTextWidth(desc, 6) / 2, 198); gfx->print(desc);
    } else if (shopCategory == SHOP_CAT_TOY) {
      char desc[32]; snprintf(desc, sizeof(desc), "喜悦感 +%u", (unsigned)p.value);
      gfx->setCursor(CX - uiTextWidth(desc, 6) / 2, 198); gfx->print(desc);
    } else if (shopCategory == SHOP_CAT_TRAVEL) {
      const char *desc = "需要燃料30 购买旅行券后出发";
      gfx->setCursor(CX - uiTextWidth(desc, 6) / 2, 198); gfx->print(desc);
    } else {
      const char *desc = shopCategory == SHOP_CAT_MEDICINE ? "根据症状恢复状态"
                        : shopCategory == SHOP_CAT_EQUIPMENT ? "提升战斗属性" : "解锁场景布置";
      gfx->setCursor(CX - uiTextWidth(desc, 6) / 2, 198); gfx->print(desc);
    }
    char info[48];
    snprintf(info, sizeof(info), "价格 %uG   库存 %s", (unsigned)p.price,
             (shopCategory == SHOP_CAT_FOOD && shopDetailSlot < 4) ? "无限" : "");
    if (!(shopCategory == SHOP_CAT_FOOD && shopDetailSlot < 4)) {
      snprintf(info, sizeof(info), "价格 %uG   库存 %u", (unsigned)p.price,
             (unsigned)pet.warehouseCount((uint8_t)shopCategory, shopDetailSlot));
    }
    gfx->setTextSize(3); gfx->setCursor(CX - uiTextWidth(info, 18) / 2, 240); gfx->print(info);
    gfx->drawRoundRect(176, 274, 114, 38, 7, UI_INK);
    gfx->setTextSize(2); gfx->setCursor(130, 282); drawNavChevron(140, 293, false, UI_INK);
    char q[6]; snprintf(q, sizeof(q), "x%u", (unsigned)shopDetailQty);
    gfx->setTextColor(UI_INK); gfx->setCursor(CX - uiTextWidth(q, 6) / 2, 282); gfx->print(q);
    drawNavChevron(326, 293, true, UI_INK);
    gfx->fillRoundRect(112, 326, 92, 40, 8, C565(0xe0,0xe7,0xee));
    gfx->fillRoundRect(262, 326, 92, 40, 8, C565(0x55,0xb3,0x83));
    drawCancelIcon(158, 346, UI_INK);
    drawConfirmIcon(308, 346, UI_WHITE);
    // 商品分页圆点与图鉴一致；左右翻页由横向滑动完成。
    for (uint8_t i = 0; i < SHOP_ITEMS_PER_CATEGORY; i++) {
      int dx = 143 + i * 10;
      if (i == shopDetailSlot) gfx->fillCircle(dx, 392, 3, UI_WHITE);
      else gfx->drawCircle(dx, 392, 2, UI_WHITE);
    }
  } else if (shopCategory < 0) {
    uint16_t colors[SHOP_CAT_COUNT] = { C565(0xc2,0x61,0x45), C565(0x72,0x6b,0xb5), C565(0x42,0xa8,0x8a), C565(0x3c,0x78,0xc2), C565(0x8c,0x62,0xb7), C565(0xd0,0x8a,0x35) };
    for (uint8_t i = 0; i < SHOP_CAT_COUNT; i++) {
      int x = (i % 2) ? 243 : 43, y = 108 + (i / 2) * 84;
      gfx->fillRoundRect(x, y, 180, 64, 10, colors[i]);
      gfx->drawRoundRect(x, y, 180, 64, 10, UI_WHITE);
      gfx->setTextColor(UI_WHITE); gfx->setTextSize(1);
      gfx->setCursor(x + (180 - uiTextWidth(SHOP_CATEGORY_NAMES[i], 6)) / 2, y + 20);
      gfx->print(SHOP_CATEGORY_NAMES[i]);
    }
  } else {
    for (uint8_t row = 0; row < 5; row++) {
      uint8_t slot = (uint8_t)(shopScroll + row);
      if (slot >= SHOP_ITEMS_PER_CATEGORY) continue;
      const ShopProduct &p = SHOP_PRODUCTS[shopCategory][slot];
      int y = 64 + row * 56;
      const int cardX = 85, cardW = 296, cardRight = cardX + cardW;
      gfx->fillRoundRect(cardX, y, cardW, 46, 9, (row & 1) ? C565(0x31,0x47,0x63) : C565(0x26,0x37,0x51));
      gfx->drawRoundRect(cardX, y, cardW, 46, 9, UI_WHITE);
      drawProductIcon((uint8_t)shopCategory, slot, 108, y + 23, 1);
      char info[12];
      snprintf(info, sizeof(info), "%uG", (unsigned)p.price);
      // Use one shared size for the name and price. Long landmark names switch
      // both sides to size 2 so their baselines remain equal without overlap.
      int rowSize = (uiTextWidth(p.name, 18) + uiTextWidth(info, 18) <= 222) ? 3 : 2;
      int asciiCell = rowSize == 3 ? 18 : 12;
      gfx->setTextColor(UI_WHITE); gfx->setTextSize(rowSize);
      const int nameX = 130;
      gfx->setCursor(nameX, y + 3); gfx->print(p.name);
      int infoWidth = uiTextWidth(info, asciiCell);
      int infoX = cardRight - 16 - infoWidth;
      gfx->setCursor(infoX, y + 3); gfx->print(info);
    }
    char page[16]; snprintf(page, sizeof(page), "%u-%u / %u", (unsigned)(shopScroll + 1), (unsigned)min((int)shopScroll + 5, SHOP_ITEMS_PER_CATEGORY), (unsigned)SHOP_ITEMS_PER_CATEGORY);
    gfx->setTextSize(1); gfx->setCursor(CX - uiTextWidth(page, 6) / 2, 366); gfx->print(page);
  }
  if (economyMsgUntil && millis() < economyMsgUntil) {
    gfx->fillRoundRect(112, 48, 242, 24, 8, economyMsgOk ? UI_BAR_OK : UI_BAR_BAD);
    gfx->setTextColor(UI_WHITE);
    gfx->setTextSize(1);
    const char *msg = economyMsgOk ? T(S_BOUGHT) : T(S_NOT_ENOUGH);
    gfx->setCursor(CX - uiTextWidth(msg, 3) / 2, 82);
    gfx->print(msg);
  }
  gfx->flush();
  cacheValid = true;
  cacheCategory = shopCategory;
  cacheDetail = shopDetailOpen;
  cacheNight = gNight;
  cacheMsgVisible = msgVisible;
  cacheMsgOk = economyMsgOk;
  cacheScroll = shopScroll;
  cacheSlot = shopDetailSlot;
  cacheQty = shopDetailQty;
  cacheLang = (uint8_t)gLang;
  cacheSession = shopSession;
  cacheCoins = coinsNow;
}

void renderTravel() {
  const ShopProduct &p = SHOP_PRODUCTS[SHOP_CAT_TRAVEL][travelSlot];
  drawScene((uint8_t)(travelSlot % 4), millis(), false);
  gfx->fillCircle(CX, CY, 231, C565(0x1d,0x2b,0x42));
  gfx->drawCircle(CX, CY, 231, UI_WHITE);
  gfx->setTextColor(UI_WHITE); gfx->setTextSize(2);
  gfx->setCursor(CX - uiTextWidth(p.name, 6) / 2, 24); gfx->print(p.name);
  gfx->fillCircle(CX, 205, 80, C565(0x3c,0x78,0xc2));
  gfx->fillCircle(CX - 36, 190, 18, C565(0x8f,0xd5,0x76));
  gfx->fillCircle(CX + 35, 200, 22, C565(0xf0,0xc3,0x55));
  gfx->setTextSize(1); gfx->setCursor(CX - 88, 306); gfx->print("旅行中: 快乐与能量已补满");
  uint32_t left = travelUntil > millis() ? travelUntil - millis() : 0;
  char sec[16]; snprintf(sec, sizeof(sec), "%us", (unsigned)((left + 999) / 1000));
  gfx->setTextSize(3); gfx->setCursor(CX - uiTextWidth(sec, 9) / 2, 338); gfx->print(sec);
  if (left == 0) travelOpen = false;
  gfx->flush();
}

void warehouseTap(int16_t x, int16_t y) {
  if (y >= 88 && y < 172) {
    uint8_t col = (uint8_t)(x / 140);
    uint8_t row = (uint8_t)((y - 88) / 42);
    uint8_t category = (uint8_t)(row * 3 + col);
    if (category < SHOP_CAT_COUNT) { shopPage = category; warehouseScroll = 0; sfxPlay(SFX_TAP); }
    return;
  }
  if (y >= 180 && y < 414) {
    uint8_t col = x < 233 ? 0 : 1;
    uint8_t row = (uint8_t)((y - 180) / 38);
    uint8_t seen = 0;
    for (uint8_t slot = 0; slot < SHOP_ITEMS_PER_CATEGORY; slot++) {
      if (!pet.warehouseCount(shopPage, slot)) continue;
      if (seen == (uint8_t)(warehouseScroll + row * 2 + col)) {
        if (shopPage == SHOP_CAT_TRAVEL) {
          if (pet.poopFuelCount() < 30) { economyNotice(false); return; }
          if (pet.useShopProduct(shopPage, slot)) {
            pet.joy = pet.energy = pet.health = 100;
            travelSlot = slot; travelUntil = millis() + 10000; travelOpen = true;
          } else economyNotice(false);
        } else if (shopPage == SHOP_CAT_EQUIPMENT) {
          // 装备必须显式点击“装备”，购买后的物品会一直留在仓库。
          economyNotice(pet.equipShopProduct(slot));
        } else economyNotice(pet.useShopProduct(shopPage, slot));
        return;
      }
      seen++;
    }
  }
}

void renderWarehouse() {
  drawScene(roomBiome(), millis(), gNight);
  gfx->fillCircle(CX, CY, 231, gNight ? C565(0x20,0x2b,0x42) : C565(0xf1,0xf4,0xf6));
  gfx->drawCircle(CX, CY, 231, UI_INK);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - uiTextWidth("仓库", 6) / 2, 18);
  gfx->print("仓库");
  for (uint8_t i = 0; i < SHOP_CAT_COUNT; i++) {
    int x = 25 + (i % 3) * 140, y = 88 + (i / 3) * 42;
    gfx->fillRoundRect(x, y, 126, 32, 8, i == shopPage ? C565(0x3c, 0x78, 0xc2) : C565(0xb0, 0xb8, 0xc4));
    gfx->setTextColor(i == shopPage ? UI_WHITE : UI_INK);
    gfx->setTextSize(1);
    gfx->setCursor(x + (126 - uiTextWidth(SHOP_CATEGORY_NAMES[i], 6)) / 2, y + 5);
    gfx->print(SHOP_CATEGORY_NAMES[i]);
  }
  uint8_t ownedCount = 0;
  for (uint8_t slot = 0; slot < SHOP_ITEMS_PER_CATEGORY; slot++) if (pet.warehouseCount(shopPage, slot)) ownedCount++;
  uint8_t pageCount = (ownedCount + 11) / 12;
  if (!pageCount) pageCount = 1;
  if (warehouseScroll >= pageCount) warehouseScroll = pageCount - 1;
  uint8_t first = warehouseScroll * 12;
  uint8_t shown = 0;
  for (uint8_t row = 0; row < 6; row++) {
    for (uint8_t col = 0; col < 2; col++) {
      uint8_t ordinal = row * 2 + col;
      uint8_t slot = 0;
      uint8_t seen = 0;
      bool found = false;
      for (slot = 0; slot < SHOP_ITEMS_PER_CATEGORY; slot++) {
        if (!pet.warehouseCount(shopPage, slot)) continue;
        if (seen++ == first + ordinal) { found = true; break; }
      }
      if (!found) continue;
      int x = 25 + col * 210, y = 180 + row * 38;
      gfx->fillRoundRect(x, y, 196, 34, 8, C565(0xe6, 0xea, 0xef));
      gfx->setTextColor(UI_INK);
      gfx->setTextSize(1);
      drawProductIcon(shopPage, slot, x + 22, y + 17, 1);
      gfx->setCursor(x + 44, y + 6);
      gfx->print(SHOP_PRODUCTS[shopPage][slot].name);
      if (shopPage == SHOP_CAT_EQUIPMENT) {
        gfx->setTextColor(UI_BAR_OK);
        gfx->setCursor(x + 148, y + 20);
        gfx->print("装备");
      } else {
        char qty[8]; snprintf(qty, sizeof(qty), "x%u", (unsigned)pet.warehouseCount(shopPage, slot));
        gfx->setCursor(x + 160, y + 9); gfx->print(qty);
      }
    }
  }
  gfx->setTextSize(1);
  char pg[16]; snprintf(pg, sizeof(pg), "%u/%u", (unsigned)(warehouseScroll + 1), (unsigned)pageCount);
  gfx->setCursor(CX - uiTextWidth(pg, 6) / 2, 414); gfx->print(pg);
  if (pageCount > 1) {
    gfx->drawLine(82, 415, 106, 415, UI_INK); gfx->drawLine(82, 415, 94, 405, UI_INK); gfx->drawLine(82, 415, 94, 425, UI_INK);
    gfx->drawLine(384, 415, 408, 415, UI_INK); gfx->drawLine(408, 415, 396, 405, UI_INK); gfx->drawLine(408, 415, 396, 425, UI_INK);
  }
  gfx->flush();
}

void renderGame() {
  // sin fillScreen(NEGRO): drawGameScene cubre los 466x466 completos. Si el
  // DMA del flush anterior aun lee el buffer, vera contenido valido (no negro
  // a medio pintar), que era el parpadeo a 25 fps.
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;

  if (gameOverUntil) {
    drawGameScene();
    if (millis() > gameOverUntil) {
      gameOpen = false;
      return;
    }
    char buf[22];
    snprintf(buf, sizeof(buf), T(S_SCORE_FMT), gameScore);
    gfx->setTextColor(ink);
    gfx->setTextSize(4);
    gfx->setCursor(CX - uiTextWidth(buf, 12) / 2, 160);
    gfx->print(buf);
    gfx->setTextSize(2);
    if (gameNewHi && gameScore > 0) {
      gfx->setTextColor(UI_BAR_WARN);
      gfx->setCursor(CX - uiTextWidth(T(S_NEW_RECORD), 6) / 2, 214);
      gfx->print(T(S_NEW_RECORD));
    } else {
      char rec[20];
      snprintf(rec, sizeof(rec), T(S_RECORD_FMT), pet.gameHi);
      gfx->setTextColor(ink);
      gfx->setCursor(CX - uiTextWidth(rec, 6) / 2, 214);
      gfx->print(rec);
    }
    const char *msg = gameScore >= 10 ? T(S_GREAT_JOY) : T(S_PLUS_JOY);
    gfx->setTextColor(ink);
    gfx->setCursor(CX - uiTextWidth(msg, 6) / 2, 250);
    gfx->print(msg);
    gfx->flush();
    return;
  }

  drawGameScene();
  stepGame();

  // marcador, record y vidas
  char buf[8];
  snprintf(buf, sizeof(buf), "%u", gameScore);
  gfx->setTextColor(ink);
  gfx->setTextSize(4);
  gfx->setCursor(CX - uiTextWidth(buf, 12) / 2, 30);
  gfx->print(buf);
  char rec[12];
  snprintf(rec, sizeof(rec), T(S_REC_FMT), pet.gameHi);
  gfx->setTextSize(2);
  gfx->setCursor(CX - uiTextWidth(rec, 6) / 2, 76);
  gfx->print(rec);
  for (int i = 0; i < 3; i++) {
    if (i < 3 - gameMisses) gfx->fillCircle(180 + i * 28, 104, 6, UI_BAR_BAD);
    else gfx->drawCircle(180 + i * 28, 104, 6, UI_TRACK);
  }

  if (pmd.loaded) {
    uint8_t act = (ballX > gamePetX + 4) ? PMD_WALKR : (ballX < gamePetX - 4) ? PMD_WALKL : PMD_IDLE;
    if (!pmd.has(act)) act = PMD_IDLE;
    drawPmdAct(act, (int)gamePetX, 394, millis(), true, false, 3);
  } else if (mon.loaded) {
    int s = (mon.h * 2 > 130) ? 1 : 2;
    int w = mon.w * s, h = mon.h * s;
    uint16_t fm = mon.frameMs ? mon.frameMs : 100;
    uint16_t fi = (millis() / fm) % mon.frames;
    const uint8_t *fr = mon.data + (uint32_t)fi * mon.w * mon.h;
    int px = (int)gamePetX - w / 2, py = 394 - h;
    for (int r = 0; r < mon.h; r++)
      for (int c = 0; c < mon.w; c++) {
        uint8_t idx = fr[r * mon.w + c];
        if (idx == 0xFF) continue;
        gfx->fillRect(px + c * s, py + r * s, s, s, mon.pal[idx]);
      }
  }

  // anillo de impacto que se expande y desvanece (feedback suave del golpe)
  uint32_t ht = millis() - hitTime;
  if (hitTime && ht < 260) {
    int rad = 22 + (int)(ht / 6);
    gfx->drawCircle((int)hitX, (int)hitY, rad, C565(0xff, 0xe7, 0x9f));
    gfx->drawCircle((int)hitX, (int)hitY, rad - 2, C565(0xff, 0xd9, 0x8a));
  }

  // la pokeball
  drawMap(SPR_ICON_PLAY, 16, (int)ballX - 24, (int)ballY - 24, 3, false);

  gfx->flush();
}

// ---------- ficha del bicho (deslizar vertical) ----------

void drawCardStat(int y, const char *label, uint16_t val, uint16_t maxBar, uint16_t color) {
  gfx->setTextColor(UI_INK);
  // Chinese glyphs are native 25px cells; match the ASCII size so label and
  // number share the same visible height and baseline.
  gfx->setTextSize(gLang == LANG_ZH ? 1 : 2);
  gfx->setCursor(96, y + 3);
  gfx->print(label);
  char num[8];
  snprintf(num, sizeof(num), "%u", val);
  gfx->setTextSize(gLang == LANG_ZH ? 3 : 2);
  gfx->setCursor(330, y + 3);
  gfx->print(num);
  int bw = 160;
  int fw = (int)val * bw / maxBar;
  if (fw > bw) fw = bw;
  gfx->fillRoundRect(150, y + 2, bw, 11, 3, UI_TRACK);
  if (fw > 2) gfx->fillRoundRect(150, y + 2, fw, 11, 3, color);
}

static void drawFuelProgress(int y) {
  const int bx = 142, bw = 204;
  uint8_t fuel = pet.poopFuelCount();
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(1);
  gfx->setCursor(64, y + 2);
  gfx->print("燃料");
  gfx->fillRoundRect(bx, y + 5, bw, 13, 5, UI_TRACK);
  int fw = (int)((uint32_t)fuel * (bw - 4) / 30);
  if (fw > 0) gfx->fillRoundRect(bx + 2, y + 7, fw, 9, 4, C565(0xd0, 0x8a, 0x35));
  char count[12];
  snprintf(count, sizeof(count), "x%u", (unsigned)pet.poopFuelBatchCount());
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(bx + bw + 10, y + 4);
  gfx->print(count);
}

// ---------- ajuste de hora en pantalla (deslizar abajo) ----------
// El usuario pone su hora LOCAL a ojo; el firmware la usa tal cual, asi que
// no hay que gestionar zona horaria. Preserva el dia (no rompe racha/edad).

void openClock() {
  uint32_t e = pet.lastSeenEpoch ? pet.lastSeenEpoch : rtcEpoch();
  clockH = (e / 3600) % 24;
  clockM = (e / 60) % 60;
  clockOpen = true;
}

void applyClock() {
  uint32_t base = pet.lastSeenEpoch ? pet.lastSeenEpoch : rtcEpoch();
  uint32_t e = (base / 86400) * 86400 + (uint32_t)clockH * 3600 + (uint32_t)clockM * 60;
  rtcSetEpoch(e);
  pet.setClock(e);
  clockOpen = false;
}

void drawClockBtn(int x, int y, const char *l) {
  gfx->fillRoundRect(x, y, 42, 42, 9, UI_WHITE);
  gfx->drawRoundRect(x, y, 42, 42, 9, UI_INK);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(x + 13, y + 11);
  gfx->print(l);
}

// pildoras de idioma centradas en y; rellena la activa
#define LANG_PILL_Y 278
#define LANG_PILL_H 36
#define LANG_PILL_X 258          // pildora de idioma (cicla los 7 al tocar)
#define LANG_PILL_W 96
static const char *const LANG_CODES[LANG_COUNT] = { "ES", "EN", "FR", "DE", "IT", "PT", "ZH" };

void renderClock() {
  gfx->fillScreen(UI_BG_DAY);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - uiTextWidth(T(S_SET_TIME), 9) / 2, 24);
  gfx->print(T(S_SET_TIME));

  char t[8];
  snprintf(t, sizeof(t), "%02d:%02d", clockH, clockM);
  gfx->setTextSize(3);
  gfx->setCursor(CX - 45, 58);
  gfx->print(t);

  drawClockBtn(118, 92, "-");  // hora -
  drawClockBtn(164, 92, "+");  // hora +
  drawClockBtn(264, 92, "-");  // min -
  drawClockBtn(310, 92, "+");  // min +
  gfx->setTextSize(1);
  gfx->setTextColor(UI_TRACK);
  gfx->setCursor(120, 138);
  gfx->print(T(S_HOUR));
  gfx->setCursor(276, 138);
  gfx->print(T(S_MIN));

  gfx->setTextColor(UI_INK); gfx->setTextSize(1); gfx->setCursor(42, 145); gfx->print("屏幕亮度");
  gfx->fillRoundRect(148, 149, 266, 12, 5, UI_TRACK);
  int knob = 150 + (int)brightnessSetting * 262 / 255;
  gfx->fillRoundRect(150, 151, knob - 150, 8, 4, C565(0x3c,0x78,0xc2));
  gfx->fillCircle(knob, 155, 9, C565(0x3c,0x78,0xc2));
  gfx->setCursor(42, 196); gfx->print("音量");
  const char *vnames[3] = { "小", "中", "大" };
  for (uint8_t i = 0; i < 3; i++) {
    int vx = 146 + i * 72;
    gfx->fillRoundRect(vx, 178, 60, 36, 8, volumeLevel == i ? C565(0x3c,0x78,0xc2) : UI_WHITE);
    gfx->drawRoundRect(vx, 178, 60, 36, 8, UI_INK);
    gfx->setTextColor(volumeLevel == i ? UI_WHITE : UI_INK); gfx->setTextSize(1);
    gfx->setCursor(vx + (60 - uiTextWidth(vnames[i], 6)) / 2, 187); gfx->print(vnames[i]);
  }

  gfx->setTextColor(UI_INK); gfx->setTextSize(1); gfx->setCursor(42, 246); gfx->print("触摸");
  const char *touchNames[3] = { "高", "中", "低" };
  uint8_t touchChoice = touchSensitivity <= 43 ? 0 : (touchSensitivity >= 68 ? 2 : 1);
  for (uint8_t i = 0; i < 3; i++) {
    int tx = 146 + i * 72;
    gfx->fillRoundRect(tx, 228, 60, 36, 8, touchChoice == i ? C565(0x3c,0x78,0xc2) : UI_WHITE);
    gfx->drawRoundRect(tx, 228, 60, 36, 8, UI_INK);
    gfx->setTextColor(touchChoice == i ? UI_WHITE : UI_INK); gfx->setTextSize(1);
    gfx->setCursor(tx + (60 - uiTextWidth(touchNames[i], 6)) / 2, 237); gfx->print(touchNames[i]);
  }

  // interruptor de sonido (izquierda de la fila de idioma)
  bool snd = audioEnabled();
  const char *sl = snd ? T(S_SND_ON) : T(S_SND_OFF);
  gfx->fillRoundRect(104, LANG_PILL_Y, 96, LANG_PILL_H, 8, snd ? UI_BAR_OK : UI_WHITE);
  gfx->drawRoundRect(104, LANG_PILL_Y, 96, LANG_PILL_H, 8, UI_INK);
  gfx->setTextColor(snd ? UI_BG_DAY : UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(104 + (96 - uiTextWidth(sl, 6)) / 2, LANG_PILL_Y + 7);
  gfx->print(sl);

  // selector de idioma: una pildora que cicla los 7 idiomas al tocar
  gfx->fillRoundRect(LANG_PILL_X, LANG_PILL_Y, LANG_PILL_W, LANG_PILL_H, 8, UI_WHITE);
  gfx->drawRoundRect(LANG_PILL_X, LANG_PILL_Y, LANG_PILL_W, LANG_PILL_H, 8, UI_INK);
  char lp[10];
  snprintf(lp, sizeof(lp), "%s >", LANG_CODES[gLang]);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(LANG_PILL_X + (LANG_PILL_W - uiTextWidth(lp, 6)) / 2, LANG_PILL_Y + 7);
  gfx->print(lp);

  bool wifiOk = WiFi.status() == WL_CONNECTED;
  gfx->fillRoundRect(56, 330, 150, 40, 10, wifiOk ? UI_BAR_OK : UI_WHITE);
  gfx->drawRoundRect(56, 330, 150, 40, 10, UI_INK);
  gfx->setTextColor(wifiOk ? UI_BG_DAY : UI_INK);
  gfx->setTextSize(1);
  const char *wifiLabel = wifiOk ? "WiFi 已连" : "连接 WiFi";
  gfx->setCursor(56 + (150 - uiTextWidth(wifiLabel, 6)) / 2, 344);
  gfx->print(wifiLabel);

  gfx->fillRoundRect(222, 330, 100, 40, 10, UI_BAR_OK);
  gfx->setTextColor(UI_BG_DAY);
  gfx->setTextColor(UI_BG_DAY);
  gfx->setTextSize(2);
  gfx->setCursor(222 + (100 - uiTextWidth("确定", 6)) / 2, 342);
  gfx->print("确定");

  gfx->fillRoundRect(330, 330, 84, 40, 10, C565(0x3c,0x78,0xc2));
  gfx->setTextColor(UI_WHITE);
  gfx->setTextSize(1);
  gfx->setCursor(330 + (84 - uiTextWidth("更新", 6)) / 2, 342);
  gfx->print("更新");

  gfx->setTextColor(UI_TRACK);
  gfx->setTextSize(1);
  if (wifiNoticeUntil && millis() < wifiNoticeUntil) {
    gfx->setCursor(CX - uiTextWidth(wifiNotice, 6) / 2, 382);
    gfx->print(wifiNotice);
  }
  // 版本号固定在设置页底部，更新按钮位于其上方。
  char ver[20];
  snprintf(ver, sizeof(ver), "v%s", FW_VERSION);
  gfx->setCursor(82, 394);
  gfx->print(ver);

  gfx->setTextColor(UI_TRACK);
  gfx->setTextSize(1);
  gfx->setCursor(CX - uiTextWidth("上滑返回", 6) / 2, 432);
  gfx->print("上滑返回");
  /*
  gfx->setTextSize(3);
  gfx->setCursor(CX - 18, 360);
  gfx->print("OK");

  gfx->setTextColor(UI_TRACK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - uiTextWidth(T(S_CLOCK_CANCEL), 6) / 2, 408);
  gfx->print(T(S_CLOCK_CANCEL));

  */
  gfx->flush();
}

void clockTap(int16_t x, int16_t y) {
  if (y >= 88 && y <= 144) {  // fila de botones +/-
    if (x >= 112 && x < 164) clockH = (clockH + 23) % 24;
    else if (x >= 164 && x < 216) clockH = (clockH + 1) % 24;
    else if (x >= 256 && x < 308) clockM = (clockM + 59) % 60;
    else if (x >= 308 && x < 360) clockM = (clockM + 1) % 60;
    return;
  }
  if (y >= 140 && y <= 170 && x >= 144 && x <= 414) {
    brightnessSetting = (uint8_t)constrain((x - 148) * 255 / 266, 20, 255);
    panel->setBrightness(brightnessSetting);
    uiPrefs.putUChar("bright", brightnessSetting);
    return;
  }
  if (y >= 174 && y <= 218) {
    for (uint8_t i = 0; i < 3; i++) {
      if (x >= 146 + i * 72 && x < 206 + i * 72) {
        volumeLevel = i;
        uiPrefs.putUChar("volume", volumeLevel);
      }
    }
    return;
  }
  if (y >= 224 && y <= 270) {
    for (uint8_t i = 0; i < 3; i++) {
      if (x >= 146 + i * 72 && x < 206 + i * 72) {
        const uint8_t values[3] = { 32, 55, 80 };
        touchSensitivity = values[i];
        uiPrefs.putUChar("touch", touchSensitivity);
      }
    }
    return;
  }
  if (y >= LANG_PILL_Y && y <= LANG_PILL_Y + LANG_PILL_H) {
    if (x >= 104 && x < 200) {                  // interruptor de sonido
      audioSetEnabled(!audioEnabled());
      if (audioEnabled()) sfxPlay(SFX_TAP);    // confirma al encender
      return;
    }
    if (x >= LANG_PILL_X && x < LANG_PILL_X + LANG_PILL_W) {  // cicla idioma
      setLang((Lang)((gLang + 1) % LANG_COUNT));
      sfxPlay(SFX_TAP);
      return;
    }
  }
  if (y >= 326 && y <= 376) {
    if (x >= 52 && x < 212) { openWifiPicker(); return; }
    if (x >= 214 && x < 326) { applyClock(); return; }
    if (x >= 326 && x <= 420) { onlineUpdate(); return; }
  }
}

// llama + numero de racha arriba a la izquierda
void drawStreakBadge() {
  if (pet.streak < 1) return;
  // At the top of the circular panel only the central ~160 px are visible.
  int x = 160, y = 14;
  gfx->fillTriangle(x + 8, y, x + 1, y + 17, x + 15, y + 17, UI_BAR_BAD);
  gfx->fillTriangle(x + 8, y + 7, x + 4, y + 17, x + 12, y + 17, UI_BAR_WARN);
  char s[6];
  snprintf(s, sizeof(s), "%u", pet.streak);
  gfx->setTextColor(inkColor());
  gfx->setTextSize(1);
  gfx->setCursor(x + 22, y + 4);
  gfx->print(s);
}

// banner temporal: medalla nueva o hito de racha
void drawCelebration() {
  const char *l1 = nullptr, *l2 = nullptr;
  char buf[20];
  if (pet.showMedal()) {
    for (int i = 0; i < MED_COUNT; i++)
      if (pet.newMedal & (1 << i)) { l2 = medalName(i); break; }
    l1 = T(S_MEDAL_BANNER);
  } else if (pet.showMilestone()) {
    snprintf(buf, sizeof(buf), T(S_STREAK_DAYS_FMT), pet.streak);
    l1 = T(S_GREAT);
    l2 = buf;
  }
  if (!l1) return;
  gfx->fillRoundRect(73, 150, 320, 96, 16, UI_BAR_WARN);
  gfx->drawRoundRect(73, 150, 320, 96, 16, UI_INK);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - uiTextWidth(l1, 9) / 2, 176);
  gfx->print(l1);
  gfx->setTextSize(2);
  gfx->setCursor(CX - uiTextWidth(l2, 6) / 2, 212);
  gfx->print(l2);
}

// medallas en la ficha: badge con etiqueta, color si conseguida
void drawMedalBadge(int x, int y, int i) {
  bool got = pet.hasMedal(1 << i);
  gfx->fillRoundRect(x, y, 100, 24, 6, got ? UI_BAR_OK : UI_TRACK);
  if (!got) gfx->drawRoundRect(x, y, 100, 24, 6, UI_TRACK);
  gfx->setTextColor(got ? UI_BG_DAY : 0x9492);
  gfx->setTextSize(2);
  gfx->setCursor(x + (100 - uiTextWidth(medalLabel(i), 12)) / 2, y + 5);
  gfx->print(medalLabel(i));
}

// pagina 0: perfil (retrato grande, identidad, racha, vinculo, baya)
void renderCardProfile() {
  const DexEntry &d = DEX_TBL[pet.speciesId];
  const char *nm = pet.nick[0] ? pet.nick : dexName(pet.speciesId);
  char head[64];
  snprintf(head, sizeof(head), T(S_NAME_FMT), pet.shiny ? "*" : "", nm, pet.level());
  gfx->setTextColor(d.accent);
  // auto-encoge: a tamano 3 los nombres largos no caben en la franja estrecha de
  // arriba de la pantalla redonda, asi que se cortaban por el borde
  int hlen = uiTextWidth(head, 9);
  int hts = (hlen <= 220) ? 3 : 2;
  gfx->setTextSize(hts);
  gfx->setCursor(CX - uiTextWidth(head, hts == 3 ? 9 : 6) / 2, hts == 3 ? 34 : 40);
  gfx->print(head);
  if (pet.nick[0]) {  // especie real bajo el apodo
    const char *sp = dexName(pet.speciesId);
    gfx->setTextColor(UI_TRACK);
    gfx->setTextSize(2);
    gfx->setCursor(CX - (uiTextWidth(sp, 6) + 12) / 2, 70);
    gfx->printf("(%s)", sp);
  }

  // retrato grande animado
  if (pmd.loaded) drawPmdAct(PMD_IDLE, CX, 206, millis(), true, false, 4);

  // racha con llama
  int sx = 138, sy = 224;
  gfx->fillTriangle(sx + 8, sy, sx + 1, sy + 18, sx + 15, sy + 18, UI_BAR_BAD);
  gfx->fillTriangle(sx + 8, sy + 7, sx + 4, sy + 18, sx + 12, sy + 18, UI_BAR_WARN);
  char rl[30];
  snprintf(rl, sizeof(rl), T(S_STREAK_FMT), pet.streak, pet.bestStreak);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(sx + 24, sy + 1);
  gfx->print(rl);

  drawCardStat(258, T(S_VIN), pet.bond, 100, C565(0xd4, 0x52, 0x7e));

  const char *berry = !pet.berryKnown ? T(S_BERRY_UNK)
                      : pet.lovesBerry(0) ? T(S_BERRY_RED)
                      : pet.lovesBerry(1) ? T(S_BERRY_BLUE)
                                          : T(S_BERRY_GREEN);
  char info[40];
  snprintf(info, sizeof(info), T(S_INFO_FMT), berry,
           (unsigned long)(pet.ageMinutes / 1440));
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - uiTextWidth(info, 9) / 2, 294);
  gfx->print(info);

  gfx->setTextColor(UI_TRACK);
  gfx->setCursor(CX - uiTextWidth(T(S_RENAME_HINT), 6) / 2, 332);
  gfx->print(T(S_RENAME_HINT));
}

// pagina 1: combate (4 barras + boton de entrenar)
void renderCardStats() {
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - uiTextWidth(T(S_BATTLE), 9) / 2, 48);
  gfx->print(T(S_BATTLE));

  drawCardStat(104, T(S_STAT_ATK), pet.atkStat(), 260, UI_BAR_BAD);
  drawCardStat(140, T(S_STAT_DEF), pet.defStat(), 260, 0x4C98);
  drawCardStat(176, T(S_STAT_SPE), pet.speStat(), 260, UI_BAR_WARN);
  drawCardStat(212, T(S_STAT_WGT), pet.weight, 100, 0xB3C8);
  drawCardStat(248, T(S_STAT_HP), pet.health, 100, UI_BAR_OK);
  drawFuelProgress(282);

  // boton: saco de entrenamiento de fuerza
  gfx->fillRoundRect(96, 322, 274, 40, 12, UI_BAR_BAD);
  gfx->setTextColor(UI_BG_DAY);
  gfx->setTextSize(2);
  gfx->setCursor(CX - uiTextWidth(T(S_TRAIN_STR), 6) / 2, 333);
  gfx->print(T(S_TRAIN_STR));
}

// pagina 2: medallas con etiqueta descriptiva
void renderCardMedals() {
  int got = 0;
  for (int i = 0; i < MED_COUNT; i++)
    if (pet.hasMedal(1 << i)) got++;
  char head[20];
  snprintf(head, sizeof(head), T(S_MEDALS_FMT), got, MED_COUNT);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - uiTextWidth(head, 9) / 2, 46);
  gfx->print(head);

  for (int i = 0; i < MED_COUNT; i++) {
    int x = 52 + (i % 2) * 192, y = 104 + (i / 2) * 54;
    bool g = pet.hasMedal(1 << i);
    gfx->fillRoundRect(x, y, 170, 44, 10, g ? UI_BAR_OK : UI_TRACK);
    if (g) {  // marca de conseguida
      gfx->fillCircle(x + 20, y + 22, 11, UI_BG_DAY);
      gfx->setTextColor(UI_BAR_OK);
      gfx->setTextSize(2);
      gfx->setCursor(x + 14, y + 13);
      gfx->print("v");
    }
    gfx->setTextColor(g ? UI_BG_DAY : 0x8410);
    gfx->setTextSize(2);
    gfx->setCursor(x + 40, y + 14);
    gfx->print(medalDesc(i));
  }
}

// pagina 3: progreso (nivel, evolucion, descuidos) — saca a la luz mecanicas
// que antes eran invisibles (cuanto falta para subir/evolucionar y por que)
void renderCardProgress() {
  const DexEntry &d = DEX_TBL[pet.speciesId];
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - uiTextWidth(T(S_PROGRESS), 9) / 2, 44);
  gfx->print(T(S_PROGRESS));

  // nivel grande
  char lv[10];
  snprintf(lv, sizeof(lv), T(S_LVL_FMT), pet.level());
  gfx->setTextSize(3);
  gfx->setCursor(CX - uiTextWidth(lv, 9) / 2, 86);
  gfx->print(lv);

  // barra de progreso al siguiente nivel (1 nivel = 60 min de juego)
  uint8_t into = pet.ageMinutes % MINUTES_PER_LEVEL;
  int bx = 93, bw = 280, by = 158, bh = 22;
  gfx->fillRoundRect(bx, by, bw, bh, 6, UI_TRACK);
  int fw = (bw - 4) * into / MINUTES_PER_LEVEL;
  if (fw > 0) gfx->fillRoundRect(bx + 2, by + 2, fw, bh - 4, 5, UI_BAR_OK);
  char nx[48];
  snprintf(nx, sizeof(nx), T(S_NEXT_LVL_FMT), MINUTES_PER_LEVEL - into, pet.level() + 1);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - uiTextWidth(nx, 9) / 2, by + 30);
  gfx->print(nx);

  // estado de evolucion
  gfx->setTextColor(UI_TRACK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - uiTextWidth(T(S_EVO_LABEL), 9) / 2, 230);
  gfx->print(T(S_EVO_LABEL));
  char evoBuf[28];
  const char *evo;
  uint16_t evoCol = UI_INK;
  if (d.evolvesTo == 0) {
    evo = T(S_FINAL_FORM);
  } else {
    int needed = d.evolveLevel + pet.careMistakes;
    if (pet.level() >= needed) {
      if (pet.lowestStat() >= 40) { evo = T(S_EVO_READY); evoCol = UI_BAR_OK; }
      else { evo = T(S_EVO_BLOCKED); evoCol = UI_BAR_BAD; }
    } else {
      snprintf(evoBuf, sizeof(evoBuf), T(S_EVO_IN_FMT), needed - pet.level());
      evo = evoBuf;
    }
  }
  gfx->setTextColor(evoCol);
  gfx->setCursor(CX - uiTextWidth(evo, 9) / 2, 256);
  gfx->print(evo);

  // descuidos (retrasan la evolucion)
  char ms[24];
  snprintf(ms, sizeof(ms), T(S_MISTAKES_FMT), pet.careMistakes);
  gfx->setTextColor(pet.careMistakes > 0 ? UI_BAR_BAD : UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - uiTextWidth(ms, 9) / 2, 310);
  gfx->print(ms);

  static const char *const tiers[5] = { "新手", "成长", "精英", "大师", "传奇" };
  char promo[40];
  if (pet.promotionTier() >= 4) snprintf(promo, sizeof(promo), "晋升 %s", tiers[pet.promotionTier()]);
  else snprintf(promo, sizeof(promo), "晋升 %s  下阶 %u级", tiers[pet.promotionTier()], pet.nextPromotionLevel());
  gfx->setTextColor(UI_BAR_BAD);
  gfx->setTextSize(2);
  gfx->setCursor(CX - uiTextWidth(promo, 6) / 2, 348);
  gfx->print(promo);
}

static const char *const EQUIP_LABELS[EQUIP_SLOT_COUNT] = {
  "头盔", "护甲", "鞋子", "左手武器", "右手武器"
};

void renderCardEquipment() {
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - uiTextWidth("装备", 9) / 2, 24);
  gfx->print("装备");
  gfx->setTextSize(1);
  gfx->setCursor(CX - uiTextWidth("点击装备可卸下", 6) / 2, 58);
  gfx->print("点击装备可卸下");
  for (uint8_t i = 0; i < EQUIP_SLOT_COUNT; i++) {
    int y = 76 + i * 50;
    gfx->drawRoundRect(28, y, 410, 42, 8, UI_INK);
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(1);
    gfx->setCursor(42, y + 11);
    gfx->print(EQUIP_LABELS[i]);
    uint8_t item = pet.equippedItem(i);
    if (item == EQUIP_EMPTY) {
      gfx->setTextColor(UI_TRACK);
      gfx->setCursor(214, y + 11);
      gfx->print("空");
      continue;
    }
    drawProductIcon(SHOP_CAT_EQUIPMENT, item, 160, y + 21, 1);
    gfx->setTextColor(UI_INK);
    gfx->setCursor(190, y + 10);
    String itemName = SHOP_PRODUCTS[SHOP_CAT_EQUIPMENT][item].name;
    if (itemName.length() > 5) itemName = itemName.substring(0, 5);
    gfx->print(itemName.c_str());
    gfx->setTextColor(UI_BAR_BAD);
    gfx->setCursor(368, y + 10);
    gfx->print("卸下");
  }
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  char bonus[48];
  snprintf(bonus, sizeof(bonus), "攻 %u  防 %u  免疫 %u", pet.equipmentAtk, pet.equipmentDef, pet.equipmentImm);
  gfx->setCursor(CX - uiTextWidth(bonus, 12) / 2, 342);
  gfx->print(bonus);
}

void renderCard() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  if (cardPage == 0) renderCardProfile();
  else if (cardPage == 1) renderCardStats();
  else if (cardPage == 2) renderCardMedals();
  else if (cardPage == 3) renderCardProgress();
  else renderCardEquipment();

  // indicador de 5 paginas + ayuda
  for (int i = 0; i < 5; i++) {
    if (i == cardPage) gfx->fillCircle(181 + i * 26, 426, 5, UI_INK);
    else gfx->drawCircle(181 + i * 26, 426, 4, UI_INK);
  }
  gfx->flush();
}

// ---------- teclado para renombrar ----------

static const char KB_KEYS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ.-";  // 28 + DEL + OK = 30
#define KB_COLS 6
#define KB_X 40
#define KB_Y 150
#define KB_W 64
#define KB_H 52

void openKeyboard() {
  kbOpen = true;
  kbMode = 0;
  strncpy(nameBuf, pet.nick, sizeof(nameBuf) - 1);
  nameBuf[sizeof(nameBuf) - 1] = 0;
  nameLen = strlen(nameBuf);
}

void renderKeyboard() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  const char *title = kbMode ? "输入 WiFi 密码" : T(S_NAME);
  gfx->setCursor(CX - uiTextWidth(title, 6) / 2, 42);
  gfx->print(title);
  if (kbMode && wifiSelected >= 0) {
    gfx->setTextSize(1);
    gfx->setCursor(CX - uiTextWidth(wifiSsids[wifiSelected], 6) / 2, 70);
    gfx->print(wifiSsids[wifiSelected]);
  }
  // buffer actual
  gfx->fillRoundRect(83, 84, 300, 40, 8, UI_WHITE);
  gfx->drawRoundRect(83, 84, 300, 40, 8, UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(95, 94);
  if (kbMode) {
    for (uint8_t i = 0; i < nameLen; ++i) gfx->print('*');
  } else {
    gfx->print(nameLen ? nameBuf : "_");
  }

  for (int i = 0; i < 30; i++) {
    int x = KB_X + (i % KB_COLS) * KB_W, y = KB_Y + (i / KB_COLS) * KB_H;
    bool special = (i >= 28);
    gfx->fillRoundRect(x, y, KB_W - 6, KB_H - 6, 6, special ? UI_BAR_WARN : UI_WHITE);
    gfx->drawRoundRect(x, y, KB_W - 6, KB_H - 6, 6, UI_INK);
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(2);
    if (kbMode && i == 27) {
      gfx->setCursor(x + 10, y + KB_H / 2 - 10);
      gfx->print("更多");
    } else if (i < 28) {
      const char *keys = wifiKbPage == 0 ? "0123456789ABCDEFGHIJKLMNOPQR"
                                         : (wifiKbPage == 1 ? "STUVWXYZabcdefghijklmnopqr"
                                                            : "stuvwxyz._-");
      if (kbMode && !keys[i]) continue;
      gfx->setCursor(x + KB_W / 2 - 9, y + KB_H / 2 - 10);
      gfx->print(kbMode ? keys[i] : KB_KEYS[i]);
    } else {
      const char *lab = (i == 28) ? (kbMode ? "返回" : "<-") : "OK";
      gfx->setCursor(x + KB_W / 2 - 15, y + KB_H / 2 - 10);
      gfx->print(lab);
    }
  }
  gfx->flush();
}

void keyboardTap(int16_t x, int16_t y) {
  int col = (x - KB_X) / KB_W, row = (y - KB_Y) / KB_H;
  if (col < 0 || col >= KB_COLS || row < 0 || row >= 5) return;
  int i = row * KB_COLS + col;
  if (i >= 30) return;
  if (kbMode && i == 27) {
    wifiKbPage = (wifiKbPage + 1) % 3;
  } else if (i == 28) {  // 返回 Wi-Fi 选择或删除一个昵称字符
    if (kbMode) { kbOpen = false; wifiPickerOpen = true; }
    else if (nameLen) nameBuf[--nameLen] = 0;
  } else if (i == 29) {  // OK
    if (kbMode) connectSelectedWifi();
    else { pet.rename(nameBuf); kbOpen = false; }
  } else if (nameLen < sizeof(nameBuf) - 1) {
    const char *keys = wifiKbPage == 0 ? "0123456789ABCDEFGHIJKLMNOPQR"
                                       : (wifiKbPage == 1 ? "STUVWXYZabcdefghijklmnopqr"
                                                          : "stuvwxyz._-");
    char key = kbMode ? keys[i] : KB_KEYS[i];
    if (!key) return;
    nameBuf[nameLen++] = key;
    nameBuf[nameLen] = 0;
  }
}

// ---------- galeria pokedex ----------

#define GAL_X 73
#define GAL_Y 84
#define GAL_CELL 80

// dibuja una miniatura centrada en su celda; sil=true la pinta en tinta
void drawThumb(const uint8_t *b, int x, int y, int s, bool sil) {
  uint8_t w = b[0], h = b[1], n = b[2];
  const uint8_t *pal = b + 3;
  const uint8_t *d = pal + n * 2;
  int ox = x + (GAL_CELL - w * s) / 2;
  int oy = y + (GAL_CELL - h * s) / 2;
  for (int r = 0; r < h; r++) {
    for (int c = 0; c < w; c++) {
      uint8_t idx = d[r * w + c];
      if (idx == 0xFF) continue;
      uint16_t col = sil ? INK_K : (uint16_t)(pal[idx * 2] | (pal[idx * 2 + 1] << 8));
      gfx->fillRect(ox + c * s, oy + r * s, s, s, col);
    }
  }
}

static void drawGalleryStatBar(const char *label, uint16_t value, uint16_t maxValue, int y, uint16_t color) {
  const int bx = 138, bw = 238;
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(1);
  gfx->setCursor(66, y + 3);
  gfx->print(label);
  gfx->fillRoundRect(bx, y + 7, bw, 14, 5, UI_TRACK);
  int fw = maxValue ? (int)((uint32_t)value * (bw - 4) / maxValue) : 0;
  if (fw > bw - 4) fw = bw - 4;
  if (fw > 0) gfx->fillRoundRect(bx + 2, y + 9, fw, 10, 4, color);
  char num[8];
  snprintf(num, sizeof(num), "%u", (unsigned)value);
  gfx->setTextColor(UI_INK);
  // 数字使用与中文标签接近的可读高度，并与进度条中心对齐。
  gfx->setTextSize(3);
  gfx->setCursor(bx + bw / 2 - uiTextWidth(num, 18) / 2, y - 3);
  gfx->print(num);
}

void renderGallery() {
  if (galleryDetail) {  // vista detalle: se redibuja siempre (animada)
    gfx->fillScreen(UI_BG_DAY);
    gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
    const DexEntry &d = DEX_TBL[galleryDetail];
    bool reg = pet.isRegistered(galleryDetail) || pet.isCaught(galleryDetail);
    char head[64];
    snprintf(head, sizeof(head), "N.%03d %s%s", galleryDetail,
             pet.isShinyRegistered(galleryDetail) ? "*" : "", reg ? dexName(galleryDetail) : "???");
    gfx->setTextColor(reg ? d.accent : UI_INK);
    int glen = uiTextWidth(head, 9);
    int gts = (glen <= 300) ? 3 : 2;  // auto-encoge nombres largos (no caben a t3)
    gfx->setTextSize(gts);
    gfx->setCursor(CX - uiTextWidth(head, gts == 3 ? 9 : 6) / 2, gts == 3 ? 56 : 60);
    gfx->print(head);
    if (galleryPmd.loaded) {
      // animado y a color si esta registrado; silueta estatica si no (estilo "?")
      drawPmdActM(galleryPmd, PMD_IDLE, CX, 208, reg ? millis() : 0, true, !reg, 5);
    } else {
      const uint8_t *t = thumbs.get(galleryDetail);
      if (t) drawThumb(t, CX - GAL_CELL, 110, 3, !reg);
    }
    // 参数采用细线框、水平长度条，数值统一放在条中线上方。
    gfx->drawRoundRect(46, 220, 374, 145, 10, UI_INK);
    drawGalleryStatBar("生命值", d.bHp, 255, 230, UI_BAR_OK);
    drawGalleryStatBar("战斗力", d.bAtk + d.bDef + d.bSpe, 260, 258, UI_BAR_BAD);
    drawGalleryStatBar("食量", 20 + d.bHp / 3, 60, 286, UI_BAR_WARN);
    drawGalleryStatBar("燃料", pet.speciesPoops(galleryDetail), 30, 314, C565(0xd0,0x8a,0x35));
    char fuelCount[12];
    snprintf(fuelCount, sizeof(fuelCount), "x%u", (unsigned)pet.speciesFuelBatches(galleryDetail));
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(2);
    gfx->setCursor(380, 316);
    gfx->print(fuelCount);
    const char *temper = (galleryDetail % 3 == 0) ? "温和" : (galleryDetail % 3 == 1) ? "活泼" : "勇敢";
    gfx->setTextColor(UI_INK); gfx->setTextSize(1);
    // The native Chinese glyph is 25px high; keep its baseline inside the
    // 220..365 parameter frame instead of letting the personality line clip.
    gfx->setCursor(CX - uiTextWidth(temper, 6) / 2, 335); gfx->print(temper);
    // 操作图标下移到状态框下方，避免与参数框重合。
    if (reg) drawSelectButton(CX, 398);
    gfx->flush();
    return;
  }

  if (!galleryDirty) return;  // la rejilla es estatica
  galleryDirty = false;

  gfx->fillScreen(UI_BG_DAY);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  char head[24];
  snprintf(head, sizeof(head), T(S_POKEDEX_FMT), pet.knownDexCount());
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - uiTextWidth(head, 9) / 2, 36);
  gfx->print(head);

  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      int16_t dex = galleryPage * 16 + r * 4 + c + 1;
      if (dex > 151) break;
      int x = GAL_X + c * GAL_CELL, y = GAL_Y + r * GAL_CELL;
      const uint8_t *t = thumbs.get(dex);
      if (t) {
        drawThumb(t, x, y, 2, !(pet.isRegistered(dex) || pet.isCaught(dex)));
        if (pet.isShinyRegistered(dex)) {
          gfx->setTextColor(UI_BAR_WARN);
          gfx->setTextSize(2);
          gfx->setCursor(x + 62, y + 4);
          gfx->print("*");
        }
      } else {
        char num[6];
        snprintf(num, sizeof(num), "%d", dex);
        gfx->setTextColor(UI_TRACK);
        gfx->setTextSize(2);
        gfx->setCursor(x + 24, y + 32);
        gfx->print(num);
      }
    }
  }
  // puntos de pagina
  for (int i = 0; i < 10; i++) {
    if (i == galleryPage) gfx->fillCircle(170 + i * 14, 448, 4, UI_INK);
    else gfx->drawCircle(170 + i * 14, 448, 3, UI_INK);
  }
  gfx->flush();
}

void galleryTap(int16_t x, int16_t y) {
  if (galleryDetail) {  // volver a la rejilla
    // The check mark is intentionally a large invisible hit target: the
    // circular display makes the thin icon itself easy to miss.
    if (y >= 352 && y <= 452 && x >= 158 && x <= 316 &&
        (pet.isRegistered(galleryDetail) || pet.isCaught(galleryDetail))) {
      pet.selectSpecies(galleryDetail);
      // 选择后立即退出图鉴并回到主页；下一轮 ensureMon 会加载新宠物图像。
      galleryOpen = false;
      galleryDetail = 0;
      galleryPmd.unload();
      galleryDirty = true;
      monFor = -2;
      sdDirty = true;
      sfxPlay(SFX_TAP);
      return;
    }
    return;
  }
  int c = (x - GAL_X) / GAL_CELL, r = (y - GAL_Y) / GAL_CELL;
  if (c < 0 || c > 3 || r < 0 || r > 3) return;
  int16_t dex = galleryPage * 16 + r * 4 + c + 1;
  if (dex > 151) return;
  galleryDetail = dex;
  galleryPmd.load(dex, pet.isShinyRegistered(dex));
}

// ---------- 野外战斗与捕捉 ----------

static BattleStats battleStatsForPet() {
  BattleStats s = {};
  s.level = pet.level();
  s.atk = pet.atkStat();
  s.def = pet.defStat();
  s.spe = pet.speStat();
  s.hp = (uint16_t)pet.health + (uint16_t)pet.equipmentImm * 4;
  if (!pet.isEgg()) {
    s.type1 = dexType1For(pet.speciesId);
    s.type2 = dexType2For(pet.speciesId);
  }
  return s;
}

void startBattle() {
  if (!canStartWildBattle(pet.isEgg(), pet.sleeping, pet.ceremony)) return;
  uint8_t wildPhase = pet.promotionTier();
  if (wildPhase > 3) wildPhase = 3;
  battleDex = pickWildSpecies((uint8_t)random(100), wildPhase);
  battleLevel = wildLevelFor(pet.level(), (uint8_t)random(100));
  battlePlayer = battleStatsForPet();
  battleEnemy = wildBattleStats(battleDex, battleLevel);
  battleRun = beginBattleRuntime(battlePlayer, battleEnemy);
  battleTurn = {};
  battleMsg[0] = 0;
  battleResolved = false;
  battleCatchOffered = false;
  battleCatchDone = false;
  battleCatchTried = false;
  battleCatchSuccess = false;
  battleSession++;
  battleDirty = true;
  battleOpen = true;
  wildPmd.unload();
  wildPmd.load((uint8_t)battleDex, false);
  sfxPlay(SFX_TAP);
}

static void finishBattle() {
  battleResolved = true;
  if (battleTurn.playerWon) {
    bool closeWin = battleRun.playerHp <= battleRun.playerMaxHp / 3;
    pet.applyBattleWin(battleDex, closeWin);
    battleCatchOffered = !pet.isCaught(battleDex);
    sfxPlay(SFX_MEDAL);
  } else {
    pet.applyBattleLoss();
    sfxPlay(SFX_DENY);
  }
}

static void performBattleAction(BattleAction action) {
  if (battleResolved) return;
  battleTurn = stepBattle(battleRun, action, (uint8_t)random(100));
  battleDirty = true;
  if (battleTurn.restFailed) snprintf(battleMsg, sizeof(battleMsg), "%s", T(S_BATTLE_REST));
  else if (battleTurn.playerRested) snprintf(battleMsg, sizeof(battleMsg), T(S_BATTLE_REST_FMT), battleTurn.playerHeal);
  else if (battleTurn.playerDamage > 0) {
    const char *kind = battleTurn.playerTypePct > 100 ? T(S_BATTLE_EFFECTIVE)
                      : battleTurn.playerTypePct < 100 ? T(S_BATTLE_WEAK) : "";
    snprintf(battleMsg, sizeof(battleMsg), "%s %u", kind, battleTurn.playerDamage);
  } else if (battleTurn.enemyDodged) snprintf(battleMsg, sizeof(battleMsg), "%s", T(S_BATTLE_DODGE));
  else if (battleTurn.playerDodged) snprintf(battleMsg, sizeof(battleMsg), "%s", T(S_BATTLE_DODGE));
  else snprintf(battleMsg, sizeof(battleMsg), "%s", T(S_BATTLE_LOSS));
  if (battleTurn.battleEnded) finishBattle();
  else sfxPlay(battleTurn.playerDamage ? SFX_PLAY : SFX_TAP);
}

void battleTap(int16_t x, int16_t y) {
  if (battleResolved) {
    if (battleCatchOffered && !battleCatchDone && y >= 370 && y <= 430) {
      if (x >= 62 && x <= 226) {
        bool closeWin = battleRun.playerHp <= battleRun.playerMaxHp / 3;
        battleCatchTried = true;
        battleCatchSuccess = pet.tryCatchWild(battleDex, battleLevel, battlePlayer.level, closeWin, (uint8_t)random(100));
        battleCatchDone = true;
        battleDirty = true;
        sfxPlay(battleCatchSuccess ? SFX_MEDAL : SFX_DENY);
      } else if (x >= 240 && x <= 404) {
        battleCatchDone = true;
        battleDirty = true;
      }
      return;
    }
    // 战斗失败或捕捉处理结束后，屏幕上的“返回”文字就是退出入口。
    // 不再依赖底部隐藏按钮，放宽纵向命中范围以适配圆屏触摸边缘。
    if ((!battleCatchOffered || battleCatchDone) && y >= 330 && y <= 420) {
      battleOpen = false;
      wildPmd.unload();
      gameMenuOpen = true;
      sfxPlay(SFX_TAP);
    }
    return;
  }
  if (y >= 344 && y <= 428) {
    if (x < 160) performBattleAction(BATTLE_ATTACK);
    else if (x < 306) performBattleAction(BATTLE_DODGE);
    else performBattleAction(BATTLE_REST);
  }
}

static void drawBattleHpArc(uint16_t cur, uint16_t maxHp, uint16_t color, bool leftSide) {
  // 两条血量各占圆屏外圈的一半：玩家沿左半圆，野外精灵沿右半圆。
  // 先铺轨道，再按比例点亮，避免直角血条挤占精灵和操作区。
  const int r = 218;
  const int thickness = 5;
  const int span = 180;
  int filled = maxHp ? (int)((uint32_t)cur * span / maxHp) : 0;
  if (filled > span) filled = span;
  for (int i = 0; i <= span; i++) {
    float deg = -90.0f + (leftSide ? -i : i);
    float a = deg * 0.0174532925f;
    int px = CX + (int)(cosf(a) * r);
    int py = CY + (int)(sinf(a) * r);
    gfx->fillCircle(px, py, thickness, UI_TRACK);
  }
  for (int i = 0; i <= filled; i++) {
    float deg = -90.0f + (leftSide ? -i : i);
    float a = deg * 0.0174532925f;
    int px = CX + (int)(cosf(a) * r);
    int py = CY + (int)(sinf(a) * r);
    gfx->fillCircle(px, py, thickness, color);
  }
}

static void drawBattleSprite(PmdMon &sprite, int dex, int cx, int ground) {
  if (sprite.loaded) {
    drawPmdActM(sprite, PMD_IDLE, cx, ground, millis(), true, false, 3);
    return;
  }
  const uint8_t *thumb = thumbs.get(dex);
  if (thumb) {
    drawThumb(thumb, cx - 40, ground - 118, 3, false);
    return;
  }
  // 最后的可见后备：精灵球和轮廓，不留下黑色空区。
  gfx->fillCircle(cx, ground - 48, 34, DEX_TBL[dex].accent);
  gfx->drawCircle(cx, ground - 48, 36, UI_INK);
  gfx->fillRect(cx - 34, ground - 50, 68, 5, UI_INK);
  gfx->fillCircle(cx, ground - 48, 10, UI_WHITE);
  gfx->drawCircle(cx, ground - 48, 10, UI_INK);
}

static int battleSpriteTop(PmdMon &sprite, int ground, uint8_t maxS) {
  if (!sprite.loaded) return ground - 118;  // thumb 后备图的固定高度
  const PmdAct &a = sprite.acts[PMD_IDLE];
  uint8_t s = a.h ? 170 / a.h : 5;
  if (s < 2) s = 2;
  if (s > maxS) s = maxS;
  while (s > 2 && a.h * s > 250) s--;
  return ground - (a.base ? a.base : a.h) * s;
}

void renderBattle() {
  static uint32_t renderedSession = 0;
  static bool renderedNight = false;
  static uint8_t renderedLang = 0;
  bool nightNow = sceneHour() < 6 || sceneHour() >= 20;
  if (!battleDirty && renderedSession == battleSession &&
      renderedNight == nightNow && renderedLang == (uint8_t)gLang) return;
  battleDirty = false;
  renderedSession = battleSession;
  renderedNight = nightNow;
  renderedLang = (uint8_t)gLang;
  drawGameScene();
  uint16_t ink = nightNow ? UI_INK_NIGHT : UI_INK;
  gfx->fillCircle(CX, CY, 226, C565(0xf3, 0xf0, 0xe7));
  gfx->drawCircle(CX, CY, 226, ink);
  gfx->setTextColor(ink);
  gfx->setTextSize(gLang == LANG_ZH ? 1 : 2);
  int cell = gLang == LANG_ZH ? 6 : 12;
  gfx->setCursor(CX - uiTextWidth(T(S_BATTLE_WILD), cell) / 2, 38);
  gfx->print(T(S_BATTLE_WILD));

  char mine[32], wild[32];
  snprintf(mine, sizeof(mine), "%s Lv.%u", pet.nick[0] ? pet.nick : dexName(pet.speciesId), battlePlayer.level);
  snprintf(wild, sizeof(wild), "%s Lv.%u", dexName(battleDex), battleLevel);
  gfx->setTextSize(gLang == LANG_ZH ? 1 : 2);
  int mineW = uiTextWidth(mine, cell);
  int wildW = uiTextWidth(wild, cell);
  int mineX = 126 - mineW / 2;
  int wildX = 340 - wildW / 2;
  if (mineX < 14) mineX = 14;
  if (wildX < 240) wildX = 240;
  if (mineX + mineW > 226) mineX = 226 - mineW;
  if (wildX + wildW > 452) wildX = 452 - wildW;
  int mineY = battleSpriteTop(pmd, 286, 3) - 35;
  int wildY = battleSpriteTop(wildPmd, 286, 3) - 35;
  if (mineY < 70) mineY = 70;
  if (wildY < 70) wildY = 70;
  gfx->setCursor(mineX, mineY); gfx->print(mine);
  gfx->setCursor(wildX, wildY); gfx->print(wild);
  drawBattleHpArc(battleRun.playerHp, battleRun.playerMaxHp, UI_BAR_OK, true);
  drawBattleHpArc(battleRun.enemyHp, battleRun.enemyMaxHp, UI_BAR_BAD, false);
  drawBattleSprite(pmd, pet.speciesId, 126, 286);
  drawBattleSprite(wildPmd, battleDex, 340, 286);

  gfx->setTextSize(gLang == LANG_ZH ? 1 : 2);
  if (battleResolved) {
    const char *res = battleTurn.playerWon ? T(S_BATTLE_WIN) : T(S_BATTLE_LOSS);
    gfx->setTextColor(battleTurn.playerWon ? UI_BAR_OK : UI_BAR_BAD);
    gfx->setCursor(CX - uiTextWidth(res, cell) / 2, 286);
    gfx->print(res);
    if (battleCatchOffered && !battleCatchDone) {
      gfx->fillRoundRect(56, 364, 170, 50, 14, UI_BAR_OK);
      gfx->fillRoundRect(240, 364, 170, 50, 14, UI_TRACK);
      gfx->setTextColor(UI_WHITE);
      gfx->setCursor(56 + (170 - uiTextWidth(T(S_BATTLE_CATCH), cell)) / 2, 377); gfx->print(T(S_BATTLE_CATCH));
      gfx->setCursor(240 + (170 - uiTextWidth(T(S_BATTLE_LEAVE), cell)) / 2, 377); gfx->print(T(S_BATTLE_LEAVE));
    } else {
      const char *msg = battleCatchTried ? (battleCatchSuccess ? T(S_BATTLE_CAUGHT) : T(S_BATTLE_ESCAPED)) : T(S_BATTLE_BACK);
      gfx->setTextColor(battleCatchTried && battleCatchSuccess ? UI_BAR_OK : ink);
      gfx->setCursor(CX - uiTextWidth(msg, cell) / 2, 360); gfx->print(msg);
    }
  } else {
    gfx->setTextColor(ink);
    gfx->setCursor(CX - uiTextWidth(battleMsg, cell) / 2, 286); gfx->print(battleMsg);
    char round[20];
    snprintf(round, sizeof(round), T(S_BATTLE_ROUND_FMT), battleRun.round + 1);
    gfx->setTextSize(3);
    gfx->setCursor(CX - uiTextWidth(round, 18) / 2, 308); gfx->print(round);
    gfx->fillRoundRect(34, 344, 126, 58, 14, C565(0xc8, 0x58, 0x4f));
    gfx->fillRoundRect(170, 344, 126, 58, 14, C565(0x4c, 0x98, 0xb0));
    gfx->fillRoundRect(306, 344, 126, 58, 14, UI_BAR_OK);
    gfx->setTextColor(UI_WHITE);
    gfx->setCursor(34 + (126 - uiTextWidth(T(S_BATTLE_ATTACK), cell)) / 2, 361); gfx->print(T(S_BATTLE_ATTACK));
    gfx->setCursor(170 + (126 - uiTextWidth(T(S_BATTLE_DODGE), cell)) / 2, 361); gfx->print(T(S_BATTLE_DODGE));
    gfx->setCursor(306 + (126 - uiTextWidth(T(S_BATTLE_REST), cell)) / 2, 361); gfx->print(T(S_BATTLE_REST));
  }
  gfx->flush();
}

void drawBattery() {
  int pc = batPercent();
  if (pc < 0) return;  // sin bateria conectada
  int x = CX - 14, y = 12, w = 24, h = 11;
  bool charging = batCharging();
  uint16_t col = charging ? UI_BAR_OK
                 : (pc >= 40) ? inkColor()
                 : (pc >= 15) ? UI_BAR_WARN
                              : UI_BAR_BAD;
  gfx->drawRoundRect(x, y, w, h, 2, col);
  gfx->fillRect(x + w, y + 3, 3, 5, col);  // borne
  if (charging) {
    // rayo de carga (zigzag) en vez de la barra de nivel
    uint16_t bolt = C565(0xff, 0xd9, 0x4a);
    int bx = x + w / 2;
    gfx->fillTriangle(bx + 3, y + 1, bx - 4, y + 6, bx + 1, y + 6, bolt);
    gfx->fillTriangle(bx - 1, y + 5, bx + 4, y + 5, bx - 3, y + 10, bolt);
  } else {
    int fw = (w - 4) * pc / 100;
    if (fw > 0) gfx->fillRect(x + 2, y + 2, fw, h - 4, col);
  }
  char pct[8];
  snprintf(pct, sizeof(pct), "%d%%", pc);
  gfx->setTextColor(col);
  gfx->setTextSize(1);
  gfx->setCursor(x + w + 8, y + 1);
  gfx->print(pct);
}

void drawHeader(const char *name, uint16_t nameColor, const char *msg) {
  drawBattery();
  gfx->drawRoundRect(374, 10, 56, 24, 6, inkColor());
  gfx->setTextColor(inkColor());
  gfx->setTextSize(1);
  gfx->setCursor(385, 14);
  gfx->print("仓库");
  gfx->setTextColor(nameColor);
  int nameCellBase = gLang == LANG_ZH ? 24 : 9;
  int nameWidth = uiTextWidth(name, nameCellBase);
  // Chinese glyphs are fixed 25px high; size 3 keeps ASCII digits at the same baseline.
  int nameSize = gLang == LANG_ZH ? (nameWidth <= 300 ? 3 : 2) : (nameWidth <= 260 ? 3 : 2);
  gfx->setTextSize(nameSize);
  int nameCell = gLang == LANG_ZH ? (nameSize == 4 ? 24 : 18) : (nameSize == 3 ? 9 : 6);
  gfx->setCursor(CX - uiTextWidth(name, nameCell) / 2, nameSize == 3 ? 52 : 58);
  gfx->print(name);
  gfx->setTextColor(inkColor());
  gfx->setTextSize(2);
  gfx->setCursor(CX - uiTextWidth(msg, 6) / 2, 90);
  gfx->print(msg);
}

// animacion de la ceremonia (10s): despedida = reverencia con corazones y se
// aleja caminando; escapada = se asusta y sale corriendo. Sustituye al idle.
void drawCeremony() {
  if (!pmd.loaded) { drawPet(); return; }  // respaldo si no hay sprite PMD
  uint32_t now = millis();
  float t = pet.ceremonyT();               // 0..1 a lo largo de los 10s
  bool panic = (pet.ceremony == CER_RUNAWAY);
  int x = CX, y = PET_GROUND;
  uint8_t act = PMD_IDLE;

  if (panic) {
    // final triste: penumbra azulada + lluvia
    for (int i = 0; i < 46; i++) {
      int rx = (i * 47 + now / 3) % 466;
      int ry = (i * 91 + now / 2) % 470;
      gfx->drawLine(rx, ry, rx - 3, ry + 12, C565(0x6a, 0x84, 0xb0));
    }
    bool fade = false;
    if (t < 0.30f) {                       // cabizbajo, temblando
      act = pmd.has(PMD_HURT) ? PMD_HURT : PMD_IDLE;
      x = CX + (int)(4 * sinf(now * 0.04f));
    } else {                               // se aleja despacio y se desvanece
      act = pmd.has(PMD_WALKL) ? PMD_WALKL : PMD_IDLE;
      x = CX - (int)(((t - 0.30f) / 0.70f) * (CX + 120));
      fade = (t > 0.6f) && ((now / 160) % 2 == 0);  // parpadea hacia la silueta
    }
    drawPmdAct(act, x, y, now, true, fade, 5);  // fade=silueta: se difumina al irse
    // lagrima cayendo del bicho
    if (t < 0.55f) {
      int ty = y - 150 + (int)((now / 6) % 40);
      gfx->fillRect(x + 6, ty, 3, 6, C565(0x9a, 0xc4, 0xe8));
    }
    return;
  }

  // despedida epica: halo dorado pulsante + chispas y corazones que ascienden
  int gcy = PET_GROUND - 96;
  for (int k = 0; k < 4; k++) {
    int r = 60 + k * 34 + (int)(10 * sinf(now * 0.02f));
    gfx->drawCircle(CX, gcy, r, C565(0xff, 0xdf, 0x8a));
  }
  for (int i = 0; i < 16; i++) {
    int px = (i * 71 + 28) % 466;
    int py = 410 - (int)((now / 8 + i * 70) % 360);   // suben y reaparecen abajo
    if (py < 30) continue;
    if (i % 4 == 0) drawMap(SPR_HEART, 32, px - 8, py - 8, 1, false);  // corazoncito
    else gfx->fillRect(px, py, 4, 4, (i % 2) ? C565(0xff, 0xe7, 0x9f) : C565(0xff, 0x9a, 0xc0));
  }

  if (t < 0.45f) {                         // reverencia / pose de despedida
    act = pmd.has(PMD_POSE) ? PMD_POSE : (pmd.has(PMD_NOD) ? PMD_NOD : PMD_IDLE);
  } else {                                 // se aleja por la derecha
    act = pmd.has(PMD_WALKR) ? PMD_WALKR : PMD_IDLE;
    x = CX + (int)(((t - 0.45f) / 0.55f) * (CX + 140));
  }
  drawPmdAct(act, x, y, now, true, false, 5);
  if (pet.showHeart())                     // corazon grande siguiendo al bicho
    drawMap(SPR_HEART, 32, x + 50, y - 190, 2, false);
}

// dialogo de decision (2 botones apilados): evolucionar/mantener o despedirse/quedaros
void drawChoiceDialog() {
  const char *q, *o1, *o2;
  uint16_t c1, c2, t1, t2;
  if (choiceKind == 1) {  // evolucion
    q = T(S_EVO_Q); o1 = T(S_EVO_TAP); o2 = T(S_EVO_KEEP);
    c1 = UI_BAR_BAD; t1 = UI_WHITE; c2 = UI_TRACK; t2 = UI_INK;
  } else {                // despedida
    q = T(S_FAR_Q); o1 = T(S_FAR_GO); o2 = T(S_FAR_STAY);
    c1 = UI_BAR_WARN; t1 = UI_INK; c2 = UI_BAR_OK; t2 = UI_WHITE;
  }
  gfx->fillRoundRect(73, 156, 320, 188, 16, UI_WHITE);
  gfx->drawRoundRect(73, 156, 320, 188, 16, UI_INK);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - uiTextWidth(q, 6) / 2, 176);
  gfx->print(q);
  gfx->fillRoundRect(93, 206, 280, 52, 12, c1);     // boton accion
  gfx->setTextColor(t1);
  gfx->setCursor(CX - uiTextWidth(o1, 6) / 2, 224);
  gfx->print(o1);
  gfx->fillRoundRect(93, 268, 280, 52, 12, c2);     // boton mantener/quedaros
  gfx->setTextColor(t2);
  gfx->setCursor(CX - uiTextWidth(o2, 6) / 2, 286);
  gfx->print(o2);
}

// boton-CTA rojo y grande para evolucionar (pulsa para llamar la atencion)
void drawEvolveButton() {
  uint32_t now = millis();
  int p = (int)(5 * sinf(now * 0.006f));  // late: -5..5
  int x = EVO_BTN_X - p, y = EVO_BTN_Y - p, w = EVO_BTN_W + 2 * p, h = EVO_BTN_H + 2 * p;
  gfx->fillRoundRect(x, y, w, h, 18, UI_BAR_BAD);
  gfx->drawRoundRect(x, y, w, h, 18, UI_WHITE);
  gfx->drawRoundRect(x + 2, y + 2, w - 4, h - 4, 16, UI_WHITE);
  gfx->setTextColor(UI_WHITE);
  gfx->setTextSize(3);
  const char *t = T(S_EVO_TAP);
  gfx->setCursor(CX - uiTextWidth(t, 9) / 2, y + h / 2 - 11);
  gfx->print(t);
}

// boton-CTA dorado de despedida: "<nombre> quiere decirte algo..."
void drawFarewellButton() {
  uint32_t now = millis();
  int p = (int)(4 * sinf(now * 0.005f));
  int x = FAR_BTN_X - p, y = FAR_BTN_Y - p, w = FAR_BTN_W + 2 * p, h = FAR_BTN_H + 2 * p;
  gfx->fillRoundRect(x, y, w, h, 16, UI_BAR_WARN);
  gfx->drawRoundRect(x, y, w, h, 16, UI_INK);
  char buf[52];
  const char *nm = pet.nick[0] ? pet.nick : dexName(pet.speciesId);
  snprintf(buf, sizeof(buf), T(S_FAREWELL_BTN), nm);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - uiTextWidth(buf, 6) / 2, y + h / 2 - 8);
  gfx->print(buf);
}

// boton-CTA sombrio de escapada por abandono: "<nombre> se siente abandonado..."
// (final triste: azul-gris oscuro, latido lento y apagado)
void drawRunawayButton() {
  uint32_t now = millis();
  int p = (int)(3 * sinf(now * 0.003f));
  int x = FAR_BTN_X - p, y = FAR_BTN_Y - p, w = FAR_BTN_W + 2 * p, h = FAR_BTN_H + 2 * p;
  gfx->fillRoundRect(x, y, w, h, 16, C565(0x3a, 0x44, 0x5a));
  gfx->drawRoundRect(x, y, w, h, 16, C565(0x70, 0x80, 0x98));
  char buf[52];
  const char *nm = pet.nick[0] ? pet.nick : dexName(pet.speciesId);
  snprintf(buf, sizeof(buf), T(S_RUNAWAY_BTN), nm);
  gfx->setTextColor(C565(0xc8, 0xd2, 0xe0));
  gfx->setTextSize(2);
  gfx->setCursor(CX - uiTextWidth(buf, 6) / 2, y + h / 2 - 8);
  gfx->print(buf);
}

// animacion epica de evolucion: halo radial + rayos giratorios + parpadeo del
// sprite acelerando + chispas que salen disparadas + fogonazo final
void drawEvolveFX(uint32_t now) {
  float t = pet.evolveT();          // 0..1
  int cx = CX, cy = PET_GROUND - 96;

  // halo radial que crece y pulsa
  int halo = 36 + (int)(t * 150) + (int)(8 * sinf(now * 0.02f));
  for (int k = 0; k < 4; k++) {
    int r = halo - k * 7;
    if (r > 0) gfx->drawCircle(cx, cy, r, UI_WHITE);
  }
  // rayos giratorios desde el centro del bicho
  float base = now * 0.004f;
  for (int i = 0; i < 12; i++) {
    float a = base + i * (float)(PI / 6);
    int len = 90 + (int)(70 * (0.5f + 0.5f * sinf(now * 0.012f + i)));
    gfx->drawLine(cx, cy, cx + (int)(cosf(a) * len), cy + (int)(sinf(a) * len), UI_WHITE);
  }
  // parpadeo entre la forma ANTERIOR y la NUEVA (siluetas), acelerando; al
  // final (t>0.9) se queda fija en la nueva para el fogonazo de revelado
  int period = 60 + (int)(220 * (1.0f - t));
  bool showOld = t < 0.9f && evoPmd.loaded && ((now / period) % 2) == 0;
  if (showOld) drawPmdActM(evoPmd, PMD_IDLE, cx, PET_GROUND, 0, true, true, 5);
  else drawPmdAct(PMD_IDLE, cx, PET_GROUND, 0, true, true, 5);
  // chispas que salen disparadas
  for (int i = 0; i < 10; i++) {
    float a = i * (float)(PI / 5) + t * 4.0f;
    int d = (int)((now / 14 + i * 33) % 200);
    int sx = cx + (int)(cosf(a) * d), sy = cy + (int)(sinf(a) * d);
    gfx->fillRect(sx - 2, sy - 2, 5, 5, (i & 1) ? C565(0xff, 0xe0, 0x70) : UI_WHITE);
  }
  // fogonazo final antes de revelar la forma nueva
  if (t > 0.9f) gfx->fillCircle(cx, cy, (int)(300 * (t - 0.9f) / 0.1f), UI_WHITE);
}

void drawPet() {
  if (pmd.loaded) {
    drawPetPMD();
    return;
  }
  if (mon.loaded) {
    drawPetSD();
    return;
  }
  int fi = flashIdxForDex(pet.speciesId);
  if (fi < 0) {
    // sin SD y sin sprite de flash: aviso claro de que faltan sprites
    gfx->setTextColor(inkColor());
    gfx->setTextSize(6);
    gfx->setCursor(CX - 18, PET_CY - 80);
    gfx->print("?");
    gfx->setTextSize(2);
    const char *l1 = T(S_NO_SPRITES);
  gfx->setCursor(CX - uiTextWidth(l1, 6) / 2, PET_CY - 4);
    gfx->print(l1);
    const char *l2 = T(S_LOAD_SPRITES);
  gfx->setCursor(CX - uiTextWidth(l2, 6) / 2, PET_CY + 26);
    gfx->print(l2);
    return;
  }
  const Species &sp = SPECIES[fi];
  int s = sp.scale;
  int x = CX - 16 * s;
  int y = PET_CY - 16 * s;

  // animacion de evolucion: alterna la silueta de la forma anterior y la nueva
  if (pet.evolving()) {
    bool flash = (millis() / 300) % 2;
    int16_t showDex = (flash && pet.prevSpeciesId >= 0) ? pet.prevSpeciesId : pet.speciesId;
    int sfi = flashIdxForDex(showDex);
    if (sfi >= 0) {
      const Species &show = SPECIES[sfi];
      drawMap(show.sprite, SPRITE_H, CX - 16 * show.scale, PET_CY - 16 * show.scale, show.scale, flash);
    }
    return;
  }

  PetMood m = pet.mood();
  if (m == MOOD_HAPPY && (millis() / 500) % 2) y -= 6;  // saltito

  drawMap(sp.sprite, SPRITE_H, x, y, s, false);

  // expresiones superpuestas usando las anclas de la especie
  bool blink = (millis() % 3500 < 300);
  if (m == MOOD_SLEEPING || blink) {
    overlayEye(sp, x, y, s, sp.eyeColL);
    overlayEye(sp, x, y, s, sp.eyeColR);
  }
  if (m == MOOD_EATING) overlayMouth(sp, x, y, s, true);
  else if (m == MOOD_SAD) overlayMouth(sp, x, y, s, false);

  if (pet.showHeart()) drawMap(SPR_HEART, 32, x + 20 * s, y - 2 * s, 2, false);
}

// ---------- escena de bano ----------

void startBath() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony || bathUntil) return;
  bathUntil = millis() + 3000;
  bathPending = true;
  int cx = (int)beh.x;
  for (auto &b : bubbles) {
    b.x = cx - 70 + random(140);
    b.y = PET_GROUND - random(150);
    b.r = 8 + random(16);
    b.ph = random(64);
  }
}

void drawBath() {
  uint32_t now = millis();
  if (now > bathUntil) {
    bathUntil = 0;
    if (bathPending) {
      bathPending = false;
      pet.clean();
      poopCleanMsgUntil = now + 2000;
      // pose de alegria al quedar limpio
      if (pmd.has(PMD_POSE)) {
        beh.mode = 2;
        beh.act = PMD_POSE;
        beh.t0 = now;
        beh.until = now + pmdActTotalMs(pmd.acts[PMD_POSE]) * 2;
      }
    }
    return;
  }
  uint32_t left = bathUntil - now;
  if (left > 800) {
    // espuma: pompas meciendose y subiendo poco a poco
    float t = now / 220.0f;
    for (auto &b : bubbles) {
      int bx = b.x + (int)(sinf(t + b.ph) * 6);
      int by = b.y - (int)((3000 - left) / 90);
      gfx->fillCircle(bx, by, b.r, UI_WHITE);
      gfx->drawCircle(bx, by, b.r, 0x7E3D);
      gfx->fillCircle(bx - b.r / 3, by - b.r / 3, b.r / 4, UI_BG_DAY);
    }
  } else {
    // las pompas revientan: destellos
    for (int i = 0; i < 8; i++) {
      auto &b = bubbles[i];
      int sx = b.x + (i % 3) * 6 - 6, sy = b.y - 18;
      uint16_t col = (i % 2) ? UI_BAR_WARN : UI_WHITE;
      gfx->fillRect(sx - 6, sy - 1, 13, 3, col);
      gfx->fillRect(sx - 1, sy - 6, 3, 13, col);
    }
  }
}

// ---------- mascota PMD: comportamiento ----------

uint32_t pmdActTotalMs(const PmdAct &a) {
  uint32_t t = 0;
  for (uint8_t i = 0; i < a.frames; i++) t += a.ms[i];
  return t ? t : 100;
}

uint8_t pmdFrameAt(const PmdAct &a, uint32_t t, bool loop) {
  uint32_t total = pmdActTotalMs(a);
  if (!loop && t >= total) return a.frames - 1;
  t %= total;
  uint8_t i = 0;
  while (t >= a.ms[i]) {
    t -= a.ms[i];
    i = (i + 1) % a.frames;
  }
  return i;
}

// dibuja una accion anclada por la base (centro-x, suelo) y devuelve su escala
// dibuja una accion de un PmdMon concreto (m); drawPmdAct usa el global pmd
void drawPmdActM(PmdMon &m, uint8_t actId, int cx, int groundY, uint32_t t, bool loop, bool sil, uint8_t maxS) {
  const PmdAct &a = m.acts[actId];
  if (!a.frames) return;
  uint8_t sBase = m.acts[PMD_IDLE].h ? 170 / m.acts[PMD_IDLE].h : 5;
  if (sBase < 2) sBase = 2;
  if (sBase > maxS) sBase = maxS;
  uint8_t s = sBase;
  while (s > 2 && a.h * s > 250) s--;  // acciones con frame grande (ataque)
  uint8_t fi = pmdFrameAt(a, t, loop);
  const uint8_t *fr = a.data + (uint32_t)fi * a.w * a.h;
  // anclar por los pies (a.base), no por el alto del lienzo: asi las acciones
  // con padding distinto (Hurt, Eat...) quedan todas a la misma altura de suelo
  int x0 = cx - a.w * s / 2, y0 = groundY - (a.base ? a.base : a.h) * s;
  for (int r = 0; r < a.h; r++) {
    const uint8_t *row = fr + r * a.w;
    for (int c = 0; c < a.w; c++) {
      uint8_t idx = row[c];
      if (idx == 0xFF) continue;
      gfx->fillRect(x0 + c * s, y0 + r * s, s, s, sil ? INK_K : m.pal[idx]);
    }
  }
}
void drawPmdAct(uint8_t actId, int cx, int groundY, uint32_t t, bool loop, bool sil, uint8_t maxS) {
  drawPmdActM(pmd, actId, cx, groundY, t, loop, sil, maxS);
}

// elige el siguiente capricho del bicho cuando esta contento
void behNext() {
  uint32_t now = millis();
  beh.t0 = now;
  int r = random(100);
  if (r < 35 && (pmd.has(PMD_WALKL) || pmd.has(PMD_WALKR))) {
    beh.mode = 1;  // paseo
    beh.targetX = 150 + random(176);
    beh.until = now + 15000;
  } else if (r < 60) {
    // gesto aleatorio entre los disponibles
    // (Hop fuera: salta demasiado alto; Sit fuera: mira hacia atras)
    static const uint8_t flair[] = { PMD_POSE, PMD_NOD, PMD_BREATH };
    uint8_t pick[3], n = 0;
    for (uint8_t f : flair)
      if (pmd.has(f)) pick[n++] = f;
    if (n) {
      beh.mode = 2;
      beh.act = pick[random(n)];
      beh.until = now + pmdActTotalMs(pmd.acts[beh.act]);
      return;
    }
    beh.mode = 0;
    beh.until = now + 2000 + random(3000);
  } else {
    beh.mode = 0;  // mirar al frente
    beh.until = now + 2000 + random(3000);
  }
}

void drawPetPMD() {
  uint32_t now = millis();

  if (pet.evolving()) {
    drawEvolveFX(now);
    return;
  }
  if (evoPmd.loaded) evoPmd.unload();  // termino la evolucion: libera la forma anterior

  PetMood m = pet.mood();
  uint8_t act;
  bool loop = true;
  if (m == MOOD_SLEEPING && pmd.has(PMD_SLEEP)) {
    act = PMD_SLEEP;
    beh.mode = 0;
  } else if (m == MOOD_EATING && pmd.has(PMD_EAT)) {
    act = PMD_EAT;
    beh.t0 = 0;
  } else if (m == MOOD_SAD && pmd.has(PMD_HURT)) {
    act = PMD_HURT;
  } else {
    // contento: el planificador decide (idle / paseo / gesto)
    if (now > beh.until) behNext();
    if (beh.mode == 1) {
      float d = beh.targetX - beh.x;
      if (fabsf(d) < 4) {
        behNext();
        act = PMD_IDLE;
      } else {
        beh.x += (d > 0 ? 3.0f : -3.0f);
        act = (d > 0) ? PMD_WALKR : PMD_WALKL;
      }
    } else {
      act = (beh.mode == 2) ? beh.act : PMD_IDLE;
      loop = false;
    }
    if (!pmd.has(act)) act = PMD_IDLE;
  }

  drawPmdAct(act, (int)beh.x, PET_GROUND, now - beh.t0, loop || act == PMD_IDLE, false, 5);

  if (pet.showHeart()) drawMap(SPR_HEART, 32, (int)beh.x + 50, PET_GROUND - 190, 2, false);
}

// sprite animado desde la SD: zoom entero por pixel, frames a su ritmo
void drawPetSD() {
  int s = mon.scale;
  int w = mon.w * s, h = mon.h * s;
  int x = CX - w / 2;
  int y = PET_CY - h / 2;

  bool sil = false;
  if (pet.evolving()) {
    sil = (millis() / 300) % 2;
  } else if (pet.mood() == MOOD_HAPPY && (millis() / 500) % 2) {
    y -= 6;  // saltito
  }

  uint16_t fm = mon.frameMs ? mon.frameMs : 100;
  uint16_t fi = pet.sleeping ? 0 : (millis() / fm) % mon.frames;
  const uint8_t *fr = mon.data + (uint32_t)fi * mon.w * mon.h;
  for (int r = 0; r < mon.h; r++) {
    const uint8_t *row = fr + r * mon.w;
    for (int c = 0; c < mon.w; c++) {
      uint8_t idx = row[c];
      if (idx == 0xFF) continue;
      gfx->fillRect(x + c * s, y + r * s, s, s, sil ? INK_K : mon.pal[idx]);
    }
  }

  // emotes en vez de expresiones (los sprites importados no tienen anclas)
  if (pet.showHeart()) drawMap(SPR_HEART, 32, x + w - 30, y - 50, 2, false);
}

// ojo cerrado: borra el ojo 3x4 y dibuja el parpado
void overlayEye(const Species &sp, int x, int y, int s, int col) {
  gfx->fillRect(x + col * s, y + sp.eyeRow * s, 3 * s, 4 * s, sp.bodyColor);
  gfx->fillRect(x + col * s, y + (sp.eyeRow + 2) * s, 3 * s, s, INK_K);
}

// borra la sonrisa base y pinta boca abierta (comer) o ceno (triste)
void overlayMouth(const Species &sp, int x, int y, int s, bool open) {
  int mc = sp.mouthCol, mr = sp.mouthRow;
  gfx->fillRect(x + (mc - 3) * s, y + mr * s, 7 * s, 2 * s, sp.bodyColor);
  if (open) {
    gfx->fillRect(x + (mc - 2) * s, y + mr * s, 5 * s, 2 * s, INK_K);
  } else {
    gfx->fillRect(x + (mc - 2) * s, y + mr * s, 5 * s, s, INK_K);
    gfx->fillRect(x + (mc - 3) * s, y + (mr + 1) * s, s, s, INK_K);
    gfx->fillRect(x + (mc + 3) * s, y + (mr + 1) * s, s, s, INK_K);
  }
}

void drawPoops() {
  for (int i = 0; i < pet.poops && i < 3; i++) {
    drawMap(SPR_POOP, 32, 36 + i * 46, 244, 2, false);
  }
  if (pet.poops > 3) {
    char count[8];
    snprintf(count, sizeof(count), "x%u", (unsigned)pet.poops);
    gfx->setTextColor(inkColor());
    gfx->setTextSize(2);
    gfx->setCursor(28, 278);
    gfx->print(count);
  }
}

void drawBars() {
  drawBar(78, 318, T(S_BAR_FOOD), pet.fullness);
  drawBar(244, 318, T(S_BAR_JOY), pet.joy);
  drawBar(78, 346, T(S_BAR_ENE), pet.energy);
  drawBar(244, 346, T(S_BAR_HYG), pet.hygiene);
}

void drawBar(int x, int y, const char *label, uint8_t val) {
  gfx->setTextColor(inkColor());
  gfx->setTextSize(2);
  gfx->setCursor(x, y);
  gfx->print(label);
  int bx = x + 58, bw = 100, bh = 15;  // espacio para etiquetas CJK de 2 caracteres
  uint16_t fill = (val >= 50) ? UI_BAR_OK : (val >= 25) ? UI_BAR_WARN : UI_BAR_BAD;
  gfx->fillRoundRect(bx, y, bw, bh, 4, UI_TRACK);
  int fw = (bw - 4) * val / 100;
  if (fw > 0) gfx->fillRoundRect(bx + 2, y + 2, fw, bh - 4, 3, fill);
}

void drawButtons() {
  for (int i = 0; i < 4; i++) {
    bool off = pet.sleeping && i != 2;  // durmiendo solo funciona LUZ
    int bx = buttons[i].cx - BTN_HALF, by = buttons[i].cy - BTN_HALF;
    if (!pet.sleeping) gfx->fillRoundRect(bx, by, 2 * BTN_HALF, 2 * BTN_HALF, 14, UI_WHITE);
    gfx->drawRoundRect(bx, by, 2 * BTN_HALF, 2 * BTN_HALF, 14, inkColor());
    if (!off) drawMap(buttons[i].icon, 16, buttons[i].cx - 16, buttons[i].cy - 16, 2, false);
  }
}

const char *eggMsg() {
  switch (pet.eggCracks()) {
    case 0: return T(S_EGG_TOUCH);
    case 1: return T(S_EGG_MOVES);
    default: return T(S_EGG_ALMOST);
  }
}

const char *statusMsg() {
  if (pet.evolving()) return T(S_EVOLVING);
  if (bathUntil) return "Splish splash!";  // onomatopeya universal
  if (pet.sleeping) return "Zzz...";
  if (pet.eating()) return T(S_EATING);
  if (pet.showHeart()) return T(S_LIKES);
  if (pet.fullness < 25) return T(S_HUNGRY);
  if (pet.hygiene < 25) return T(S_NEEDS_BATH);
  if (pet.energy < 25) return T(S_EXHAUSTED);
  if (pet.health < 25) return T(S_SAD);
  if (pet.joy < 25) return T(S_SAD);
  if (pet.weight > 60) return T(S_CHUBBY);
  if (pet.shiny && pet.ageMinutes < 15) return T(S_IS_SHINY);
  return T(S_HAPPY);
}

// dibuja un mapa de n x n pixeles escalado; silhouette=true lo pinta en tinta
void drawMap(const char *const *map, int n, int x, int y, int s, bool silhouette) {
  for (int r = 0; r < n; r++) {
    for (int c = 0; c < n; c++) {
      char ch = map[r][c];
      if (ch == '.') continue;
      gfx->fillRect(x + c * s, y + r * s, s, s, silhouette ? INK_K : spriteColor(ch));
    }
  }
}
