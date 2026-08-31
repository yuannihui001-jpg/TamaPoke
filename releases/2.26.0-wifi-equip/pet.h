#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include "shop.h"

// 1 tick = 1 minuto de juego. Baja este valor para probar mas rapido
// (p. ej. 5000UL = las estadisticas caen 12x mas rapido).
#define PET_TICK_MS 60000UL
// Minutos de juego por nivel. Con 60, CHARMANDER evoluciona a las ~16 h
// de juego con cuidado perfecto. Baja a 1 para ver evoluciones al momento.
#define MINUTES_PER_LEVEL 60
#define EAT_ANIM_MS 2500UL
#define HEART_MS 1500UL
#define EVOLVE_ANIM_MS 5200UL              // animacion de evolucion (mas larga = mas epica)
#define CEREMONY_MS 10000UL                // duracion de la despedida en pantalla
#define FAREWELL_AGE_MIN (3UL * 24 * 60)   // se despide a los 3 dias de juego (en forma final)
#define RUNAWAY_TICKS 60                   // se escapa tras 1 h con TODO a cero

// 装备槽位：头盔、护甲、鞋子，以及左右手武器。
enum PetEquipmentSlot : uint8_t {
  EQUIP_HELMET = 0,
  EQUIP_ARMOR,
  EQUIP_SHOES,
  EQUIP_LEFT_HAND,
  EQUIP_RIGHT_HAND,
  EQUIP_SLOT_COUNT,
};
#define EQUIP_EMPTY 255

// ceremonias de fin de ciclo
enum : uint8_t { CER_NONE = 0, CER_FAREWELL, CER_RUNAWAY, CER_RELEASE };

enum PetMood : uint8_t { MOOD_HAPPY, MOOD_SAD, MOOD_EATING, MOOD_SLEEPING };

enum ShopItem : uint8_t {
  SHOP_BERRY_RED = 0,
  SHOP_BERRY_BLUE,
  SHOP_BERRY_GREEN,
  SHOP_CANDY,
  SHOP_TOY,
  SHOP_MEDICINE,
  SHOP_TRAIN_TOKEN,
  SHOP_TOY_BOX,
  SHOP_ITEM_COUNT,
};

// medallas del individuo (bitmask)
enum : uint16_t {
  MED_LV10 = 1 << 0, MED_LV25 = 1 << 1, MED_LV50 = 1 << 2,
  MED_BERRY = 1 << 3, MED_STREAK7 = 1 << 4, MED_BOND = 1 << 5,
  MED_FINAL = 1 << 6, MED_FIT = 1 << 7,
};
#define MED_COUNT 8

class Pet {
public:
  // Estadisticas 0..100
  uint8_t fullness = 80;  // comida
  uint8_t joy = 80;       // felicidad
  uint8_t energy = 80;    // energia
  uint8_t hygiene = 100;  // limpieza
  uint8_t health = 100;   // salud (mecanica TamaPetchi)
  uint8_t poops = 0;      // cacas pendientes de limpiar (se muestran como maximo 3)
  uint8_t poopFuel = 0;   // combustible permanente obtenido al limpiar (0..30)
  uint16_t fuelBatches = 0; // cada 30 unidades completadas = 1 viaje disponible
  uint8_t weight = 0;     // 0-100: las chuches engordan, el minijuego quema
  uint16_t coins = 20;    // moneda del mundo: juegos y entrenamiento la generan
  uint8_t room = 0;       // 0 casa, 1 parque, 2 playa, 3 bosque
  uint8_t decorOwned = 1; // ocho juguetes: pelota, flores, tienda, lampara, tambor, bloques, tren, cometa
  uint8_t decorPlaced[4] = { 1, 0, 0, 0 }; // objetos colocados por sala
  uint8_t warehouse[SHOP_TOTAL_ITEMS] = { 0 }; // compras pendientes de usar
  uint16_t propOwned = 0;   // decoracion comprada en la tienda de objetos
  uint16_t propPlaced[4] = { 0, 0, 0, 0 };
  uint8_t equipmentAtk = 0, equipmentDef = 0, equipmentImm = 0;
  uint8_t equipped[EQUIP_SLOT_COUNT] = { EQUIP_EMPTY, EQUIP_EMPTY, EQUIP_EMPTY, EQUIP_EMPTY, EQUIP_EMPTY };
  // genes (90-110%, se tiran al eclosionar) y entrenamiento (0-100)
  uint8_t geneAtk = 100, geneDef = 100, geneSpe = 100;
  uint8_t trAtk = 0, trDef = 0, trSpe = 0;
  bool berryKnown = false;  // ya descubrio su baya favorita
  bool shiny = false;       // variante de color rara (se sortea en el huevo)
  uint32_t ageMinutes = 0;
  int16_t speciesId = -1;      // numero de Pokedex (1-151), -1 = huevo
  int16_t prevSpeciesId = -1;  // para la animacion de evolucion
  uint8_t careMistakes = 0;   // descuidos: cada uno retrasa la evolucion 1 nivel
  bool sleeping = false;
  uint32_t lastSeenEpoch = 0;   // ultima hora RTC vista (para progresion offline)
  uint8_t ceremony = CER_NONE;  // despedida/escapada/liberacion en curso
  uint8_t lastEnd = CER_NONE;   // como acabo la anterior (afecta al huevo)
  uint8_t dexReg[19] = { 0 };       // pokedex de criados (bitmap 151 bits)
  uint8_t dexShinyReg[19] = { 0 };  // criados en version shiny
  uint8_t dexCaught[19] = { 0 };    // pokedex de salvajes capturados
  // racha de cuidado diario (del jugador: persiste entre crianzas)
  uint16_t streak = 0, bestStreak = 0;
  uint32_t lastCareDay = 0;
  // vinculo (del bicho: sube lento con cuidado, se resetea al nacer otro)
  uint8_t bond = 0;
  char nick[12] = "";    // apodo (vacio = nombre de especie)
  // medallas: del individuo + contador acumulado entre todas las crianzas
  uint16_t medals = 0, totalMedals = 0;
  uint16_t newMedal = 0;   // recien conseguida(s), para celebrar
  uint16_t lastMilestone = 0;  // hito de racha ya celebrado
  uint16_t gameHi = 0;     // record del minijuego (del jugador)
  uint16_t memoryHi = 0;   // mejor puntuacion del juego de memoria
  uint16_t strHi = 0;      // record de golpes al saco
  uint16_t battleWins = 0, battleLosses = 0;
  uint16_t battleStreak = 0, bestBattleStreak = 0;
  int16_t activeSpeciesId = -1; // 当前图鉴中选择饲养的伙伴；其余记录保持暂停
  uint8_t dailyFlags = 0;        // 今日任务：喂食/清洁/游戏/战斗

  void begin();                 // carga estado de NVS (o crea el primer huevo)
  void update(uint32_t nowMs);  // llamar en cada loop()

  // Acciones (botones tactiles)
  void feed();              // baya roja (compatibilidad)
  void feedBerry(uint8_t color);  // 0 roja, 1 azul, 2 verde
  void feedCandy();
  bool lovesBerry(uint8_t color) const {
    return !isEgg() && (speciesId % 3) == color;  // gusto oculto por especie
  }
  void playResult(uint8_t score);  // recompensa del minijuego (entrena VEL)
  void memoryResult(uint8_t score, bool won);  // recompensa del juego de memoria
  uint8_t trainStrength(uint16_t hits);  // saco de entrenamiento (entrena FUE)
  uint16_t knownDexCount() const;
  uint16_t caughtCount() const;
  bool isCaught(int16_t dex) const {
    return dex >= 1 && dex <= 151 && (dexCaught[(dex - 1) >> 3] & (1 << ((dex - 1) & 7)));
  }
  void registerCaught(int16_t dex);
  uint8_t catchChanceForWild(int16_t wildDex, uint8_t wildLevel, uint8_t petLevel, bool closeWin) const;
  bool tryCatchWild(int16_t wildDex, uint8_t wildLevel, uint8_t petLevel, bool closeWin, uint8_t luckRoll);
  void applyBattleWin(int16_t wildDex, bool closeWin);
  void applyBattleLoss();
  void earnCoins(uint16_t amount);
  bool buyItem(uint8_t item);
  bool buyShopProduct(uint8_t category, uint8_t slot);
  bool useShopProduct(uint8_t category, uint8_t slot);
  bool equipShopProduct(uint8_t slot);
  bool unequipSlot(uint8_t equipSlot);
  uint8_t equippedItem(uint8_t equipSlot) const {
    return equipSlot < EQUIP_SLOT_COUNT ? equipped[equipSlot] : EQUIP_EMPTY;
  }
  bool selectSpecies(int16_t dex);
  uint8_t speciesPoops(int16_t dex);
  uint8_t poopFuelCount() const { return poopFuel; }
  uint16_t poopFuelBatchCount() const { return fuelBatches; }
  uint16_t speciesFuelBatches(int16_t dex);
  bool isActiveSpecies(int16_t dex) const { return activeSpeciesId == dex; }
  uint8_t warehouseCount(uint8_t category, uint8_t slot) const {
    // Four staple foods are part of the starter pantry and never run out.
    if (category == SHOP_CAT_FOOD && slot < 4) return 255;
    return (category < SHOP_CAT_COUNT && slot < SHOP_ITEMS_PER_CATEGORY)
      ? warehouse[shopProductId(category, slot)] : 0;
  }
  bool toggleProp(uint8_t slot);
  uint16_t roomProps() const { return propPlaced[room < 4 ? room : 0]; }
  bool visitRoom(uint8_t nextRoom);
  bool toggleDecor(uint8_t slot);
  uint8_t roomDecor() const { return decorPlaced[room < 4 ? room : 0]; }

  // stats de combate: base real de gen 1 x genes + nivel + entrenamiento
  uint16_t atkStat() const;
  uint16_t defStat() const;
  uint16_t speStat() const;
  void play();
  void toggleLight();  // dormir / despertar
  void clean();
  void markDaily(uint8_t bit) { dailyFlags |= bit; }
  uint8_t dailyProgress() const {
    uint8_t n = 0; for (uint8_t b = 1; b <= 8; b <<= 1) if (dailyFlags & b) n++; return n;
  }
  void caress();  // tocar al bicho
  void eggTap();  // tocar el huevo: 3 toques y eclosiona
  void newEgg();   // empezar de cero con un inicial aleatorio
  void release();  // soltar (pulsacion larga + confirmar)
  void syncClock(uint32_t nowEpoch);  // aplica el tiempo transcurrido apagado
  void setClock(uint32_t nowEpoch);   // fija la hora sin aplicar progresion
  void startFarewell();  // tambien usable desde la consola serie (BYE)
  void startRunaway();   // tambien usable desde la consola serie (RUN)

  bool isEgg() const { return speciesId < 0; }
  uint8_t eggCracks() const { return eggTaps; }
  bool eating() const { return millis() < eatUntil; }
  bool showHeart() const { return millis() < heartUntil; }
  bool evolving() const { return millis() < evolveUntil; }
  float evolveT() const {     // progreso de la animacion de evolucion 0..1
    uint32_t n = millis();
    uint32_t left = evolveUntil > n ? evolveUntil - n : 0;
    return 1.0f - (float)left / (float)EVOLVE_ANIM_MS;
  }
  bool canEvolveNow() const;  // condiciones de evolucion cumplidas (lista)
  void evolve();              // dispara la transformacion (la llama un toque del usuario)
  bool canFarewellNow() const;  // forma final + 7 dias: lista para despedirse (boton)
  bool canRunawayNow() const;   // abandono total 1h: lista para escaparse (boton triste)
  // el usuario decide en un dialogo; "mantener/quedaros" pospone y re-ofrece luego
  bool wantEvolveButton() const { return canEvolveNow() && level() > evoDeclinedLv; }
  bool wantFarewellButton() const { return canFarewellNow() && ageMinutes >= farDeclinedAge; }
  void declineEvolve() { evoDeclinedLv = level(); }              // re-ofrece al subir de nivel
  void declineFarewell() { farDeclinedAge = ageMinutes + 1440; } // re-ofrece dentro de 1 dia
  // primera partida: el jugador elige inicial (Bulbasaur/Charmander/Squirtle)
  bool awaitingStarter() const { return starterPick; }
  void chooseStarter(int16_t dex) { eggTarget = dex; starterPick = false; save(); }
  void factoryReset() { prefs.clear(); }  // borra la NVS (test: comando serie WIPE)
  void dbgRunawayReady() { fullness = joy = energy = hygiene = 0; neglectTicks = RUNAWAY_TICKS; }  // test
  uint8_t level() const { return 1 + ageMinutes / MINUTES_PER_LEVEL; }
  // 晋升阶段随等级解锁，供成长页和战斗匹配使用：新手、成长、精英、大师、传奇。
  uint8_t promotionTier() const {
    uint8_t lv = level();
    return lv >= 100 ? 4 : lv >= 50 ? 3 : lv >= 25 ? 2 : lv >= 10 ? 1 : 0;
  }
  uint8_t nextPromotionLevel() const {
    static const uint8_t LEVELS[5] = { 1, 10, 25, 50, 100 };
    uint8_t tier = promotionTier();
    return tier >= 4 ? 100 : LEVELS[tier + 1];
  }
  bool isRegistered(int16_t dex) const {
    return dex >= 1 && dex <= 151 && (dexReg[(dex - 1) >> 3] & (1 << ((dex - 1) & 7)));
  }
  bool isShinyRegistered(int16_t dex) const {
    return dex >= 1 && dex <= 151 && (dexShinyReg[(dex - 1) >> 3] & (1 << ((dex - 1) & 7)));
  }
  uint16_t registeredCount() const;
  bool lineHasUnregistered(int16_t base) const;
  uint8_t eggRarity() const;       // rareza del huevo actual (sin revelar especie)
  int16_t pickEggSpecies();        // publica para poder simular tiradas (EGGS)
  uint8_t lowestStat() const { return min(min(fullness, joy), min(energy, hygiene)); }
  PetMood mood() const;
  // progreso de la ceremonia de despedida/escapada, 0..1 (para animarla)
  float ceremonyT() const {
    if (ceremony == CER_NONE) return 0.0f;
    uint32_t n = millis();
    uint32_t left = ceremonyUntil > n ? ceremonyUntil - n : 0;
    return 1.0f - (float)left / (float)CEREMONY_MS;
  }

  // racha / vinculo / medallas / nombre
  void rename(const char *name);
  bool hasMedal(uint16_t m) const { return medals & m; }
  bool showMedal() const { return millis() < medalUntil; }
  bool showMilestone() const { return millis() < milestoneUntil; }
  int careBonus() const;  // mejora del huevo por racha + vinculo

  // guardado periodico diferido: tick() marca pendiente y el loop lo vuelca
  // cuando la pantalla esta atenuada/apagada (la escritura a flash congela
  // ~1s ambos cores: asi no se ve ni corta el tactil)
  bool savePending() const { return pendingSave; }
  void flushSave();

private:
  Preferences prefs;
  uint32_t lastTick = 0;
  uint32_t eatUntil = 0;
  uint32_t heartUntil = 0;
  uint32_t evolveUntil = 0;
  int16_t eggTarget = 1;       // dex oculto que saldra del huevo
  bool eggShiny = false;       // sorpresa sorteada al crear el huevo
  uint8_t eggTaps = 0;
  uint8_t mistakeCooldown = 0;
  uint8_t ticksSinceSave = 0;
  bool pendingSave = false;     // guardado periodico pendiente de volcar
  uint8_t evoDeclinedLv = 0;    // "mantener forma": no ofrecer evolucion hasta subir de nivel
  uint32_t farDeclinedAge = 0;  // "quedaros juntos": no ofrecer despedida hasta esta edad
  bool starterPick = false;     // primera partida: esperando que el jugador elija inicial
  uint8_t neglectTicks = 0;
  uint16_t goodTicks = 0;  // racha bien cuidado: forja la DEF
  uint32_t ceremonyUntil = 0;
  uint8_t bondToday = 0;       // tope diario de subida de vinculo
  uint32_t medalUntil = 0;     // celebracion de medalla en pantalla
  uint32_t milestoneUntil = 0; // celebracion de hito de racha

  uint32_t today() const { return lastSeenEpoch ? lastSeenEpoch / 86400 : 0; }
  void registerCare();   // primer cuidado del dia: racha + vinculo
  void addBond(uint8_t amt);
  void checkMedals();
  void tick();
  void hatch();
  void registerSpecies(int16_t dex);
  void saveActiveSlot();
  bool loadActiveSlot(int16_t dex);
  void save();
  void load();
  static uint8_t clamp100(int v) { return v < 0 ? 0 : (v > 100 ? 100 : v); }
};
