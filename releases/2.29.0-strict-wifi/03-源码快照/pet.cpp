#include "pet.h"
#include "dex.h"
#include "audio.h"

void Pet::begin() {
  prefs.begin("tamapoke", false);
  if (!prefs.getBool("init", false)) {
    prefs.putBool("init", true);
    newEgg();
  } else {
    load();
  }
  lastTick = millis();
}

void Pet::newEgg() {
  ceremony = CER_NONE;
  neglectTicks = 0;
  weight = 0;
  speciesId = -1;
  prevSpeciesId = -1;
  eggTarget = pickEggSpecies();  // especie oculta segun rareza y pokedex
  starterPick = (registeredCount() == 0);  // primera partida: el jugador elige inicial
  // sorteo shiny: 1/48 base, mejor con despedida y con racha/vinculo altos
  int shinyBase = (lastEnd == CER_FAREWELL ? 24 : 48) - careBonus();
  if (shinyBase < 8) shinyBase = 8;
  eggShiny = (random(shinyBase) == 0);
  eggTaps = 0;
  fullness = 80;
  joy = 80;
  energy = 80;
  hygiene = 100;
  health = 100;
  poops = 0;
  poopFuel = 0;
  ageMinutes = 0;
  careMistakes = 0;
  mistakeCooldown = 0;
  sleeping = false;
  equipmentAtk = equipmentDef = equipmentImm = 0;
  for (uint8_t i = 0; i < EQUIP_SLOT_COUNT; i++) equipped[i] = EQUIP_EMPTY;
  save();
}

// progresion offline: el tiempo paso aunque estuviera apagado, pero con
// piedad — las barras bajan con suelo en 15 (vuelve hambriento, no muerto),
// sin descuidos ni escapadas en ausencia
static uint8_t dropTo(uint8_t v, uint8_t d, uint8_t fl) {
  if (v <= fl) return v;
  return (v - fl > d) ? v - d : fl;
}

void Pet::setClock(uint32_t nowEpoch) {
  lastSeenEpoch = nowEpoch;
  if (nowEpoch) save();  // persiste ya: un corte de luz no pierde la referencia
}

void Pet::syncClock(uint32_t nowEpoch) {
  uint32_t seen = prefs.getUInt("seen", 0);
  lastSeenEpoch = nowEpoch;
  if (nowEpoch == 0) return;
  uint32_t mins = (seen && nowEpoch > seen) ? (nowEpoch - seen) / 60 : 0;
  if (mins < 2 || ceremony != CER_NONE || starterPick) {
    save();  // primera vez, sin tiempo que aplicar o aun eligiendo inicial
    return;
  }
  if (mins > 14UL * 24 * 60) mins = 14UL * 24 * 60;  // tope: 2 semanas

  for (uint32_t i = 0; i < mins; i++) {
    ageMinutes++;
    if (isEgg()) {
      if (ageMinutes >= 3) hatch();  // eclosiona en tu ausencia
      continue;
    }
    if (sleeping) {  // descanso: baja lento y con suelo, igual que en vivo
      energy = clamp100(energy + 6);
      if (ageMinutes % 2 == 0) {
        fullness = dropTo(fullness, 1, 30);
        joy = dropTo(joy, 1, 35);
      }
      if (ageMinutes % 3 == 0) hygiene = dropTo(hygiene, 1, 45);
      continue;
    }
    fullness = dropTo(fullness, 2, 15);
    energy = dropTo(energy, 1, 15);
    hygiene = dropTo(hygiene, 1, 15);
    joy = dropTo(joy, 1, 15);
    if (fullness < 20 || hygiene < 20) health = dropTo(health, 1, 1);
    else if (health < 100 && fullness >= 60 && hygiene >= 60 && ageMinutes % 10 == 0)
      health = clamp100(health + 1);
  }
  if (!isEgg()) {
    if (!sleeping) {  // durmiendo no ensucia
      uint16_t p = (uint16_t)poops + mins / 240;
      poops = p > 30 ? 30 : (uint8_t)p;
    }
    // la evolucion NO se aplica offline: queda lista y la dispara el usuario
    // tocando al bicho cuando vuelve (para que vea la transformacion)
  }
  Serial.printf("offline: %u min aplicados (nv.%u)\n", mins, level());
  save();
}

void Pet::update(uint32_t nowMs) {
  // fin de ceremonia: la criatura se va y queda un huevo nuevo
  if (ceremony != CER_NONE && millis() > ceremonyUntil) {
    newEgg();
    return;
  }
  while (nowMs - lastTick >= PET_TICK_MS) {
    lastTick += PET_TICK_MS;
    tick();
  }
}

void Pet::tick() {
  if (ceremony != CER_NONE) return;  // el tiempo se detiene en la despedida
  if (starterPick) return;  // la partida no empieza hasta elegir inicial: si el
                            // tiempo corriera aqui, el huevo eclosionaria solo a
                            // los 3 min con la especie sorteada y se perderia la
                            // eleccion del jugador
  ageMinutes++;

  if (isEgg()) {
    if (ageMinutes >= 3) hatch();  // si no lo tocas, eclosiona solo a los 3 min
    return;
  }

  // el sueño es descanso: la energia se recupera y las necesidades bajan MUCHO
  // mas lento que despierto y con suelo (amanece pidiendo algo de mimo, no a
  // cero, sin descuidos ni escapadas). despierto: comida -2/min, hig/joy -1/min.
  // El peso aun se quema; la racha de buen cuidado (goodTicks) queda en pausa.
  if (sleeping) {
    energy = clamp100(energy + 6);
    if (weight > 0 && ageMinutes % 3 == 0) weight--;
    if (ageMinutes % 2 == 0) {                 // ~4x mas lento que despierto
      fullness = dropTo(fullness, 1, 30);
      joy = dropTo(joy, 1, 35);
    }
    if (ageMinutes % 3 == 0) hygiene = dropTo(hygiene, 1, 45);
    checkMedals();  // aun puede cruzar un nivel por edad mientras duerme
    if (++ticksSinceSave >= 5) pendingSave = true;
    return;
  }

  if (ageMinutes % MINUTES_PER_LEVEL == 0) sfxPlay(SFX_LEVEL);  // subio de nivel (despierto)

  fullness = clamp100(fullness - 2);
  energy = clamp100(energy - 1);
  if (fullness > 40 && poops < 30 && random(100) < 15) poops++;

  hygiene = clamp100(hygiene - 1 - 4 * poops);
  // el sobrepeso da pereza: la energia cae el doble
  if (weight > 50) energy = clamp100(energy - 1);
  if (weight > 0 && ageMinutes % 3 == 0) weight--;

  // la disciplina forja la defensa: 12 h seguidas bien cuidado = +1 DEF
  if (lowestStat() >= 40) {
    if (++goodTicks >= 720) {
      goodTicks = 0;
      if (trDef < 100) trDef++;
    }
  } else {
    goodTicks = 0;
  }

  int dJoy = -1;
  if (fullness < 30) dJoy -= 2;
  if (hygiene < 30) dJoy -= 2;
  joy = clamp100(joy + dJoy);

  // TamaPetchi-style health loop: neglecting food or cleanliness makes the
  // pet sick; good care slowly restores health without introducing an
  // automatic death that would conflict with TamaPoke's existing endings.
  if (fullness < 20 || hygiene < 20) health = dropTo(health, 1, 1);
  else if (health < 100 && fullness >= 60 && hygiene >= 60 && ageMinutes % 10 == 0)
    health = clamp100(health + 1);

  // Descuido: dejar una estadistica por los suelos cuenta como error de
  // cuidado (con enfriamiento para no contar el mismo descuido cada minuto)
  if (mistakeCooldown > 0) mistakeCooldown--;
  if (lowestStat() <= 10 && mistakeCooldown == 0) {
    careMistakes++;
    mistakeCooldown = 60;
    if (bond > 1) bond--;  // el descuido enfria el vinculo, pero sin arrasarlo:
                           // a -3 cada 30 min se perdia mucho mas de lo que se
                           // podia ganar en todo un dia y el vinculo se atascaba
  }

  checkMedals();  // la evolucion la dispara el usuario (canEvolveNow + tap), no el tick

  // abandono total: con TODO a cero durante una hora queda lista para escaparse;
  // NO se va sola, la dispara el usuario con el boton (final triste, lo presencia)
  if (fullness == 0 && joy == 0 && energy == 0 && hygiene == 0) {
    if (neglectTicks < RUNAWAY_TICKS) neglectTicks++;
  } else {
    neglectTicks = 0;  // un solo cuidado la salva
  }

  // ciclo completo (forma final + 7 dias): la despedida NO salta sola; queda
  // lista (canFarewellNow) y la dispara el usuario con el boton, para que la vea

  // autoguardado periodico: NO escribir a flash aqui (corre dentro del loop,
  // mientras se anima); solo marcar y dejar que el loop lo vuelque al atenuar
  if (++ticksSinceSave >= 5) pendingSave = true;
}

// vuelca el guardado periodico pendiente (lo llama el loop en un momento sin
// animacion para que el paron de la escritura a flash no se vea)
void Pet::flushSave() {
  if (pendingSave) save();
}

// quedan miembros sin registrar en la linea evolutiva de esta base?
bool Pet::lineHasUnregistered(int16_t base) const {
  int16_t cur = base;
  for (int guard = 0; cur >= 1 && cur <= 151 && guard < 6; guard++) {
    if (!isRegistered(cur)) return true;
    if (cur == DEX_EEVEE) {
      for (int16_t b = 134; b <= 136; b++)
        if (!isRegistered(b)) return true;
      return false;
    }
    cur = DEX_TBL[cur].evolvesTo;
  }
  return false;
}

uint8_t Pet::eggRarity() const {
  return (eggTarget >= 1 && eggTarget <= 151) ? DEX_TBL[eggTarget].rarity : R_COMUN;
}

// elige la especie del huevo: tirada de rareza (mejorada por una despedida
// completa, castigada por una escapada) y sesgo hacia lineas incompletas
int16_t Pet::pickEggSpecies() {
  // primera partida: inicial clasico
  if (registeredCount() == 0) {
    return CLASSIC_DEX[random(NUM_CLASSIC_DEX)];
  }

  uint8_t tier = R_COMUN;
  if (lastEnd != CER_RUNAWAY) {
    bool blessed = (lastEnd == CER_FAREWELL);
    int rare = (blessed ? 45 : 27) + careBonus();
    int leg = (registeredCount() >= 25) ? (blessed ? 10 : 3) + careBonus() / 3 : 0;
    int r = random(100);
    if (r < leg) tier = R_LEGENDARIO;
    else if (r < leg + rare) tier = R_RARO;
  }

  // candidatos del tier con linea incompleta; si no hay, baja de tier;
  // si la pokedex del tier esta completa, vale cualquiera del tier
  for (int pass = 0; pass < 2; pass++) {
    for (int t = tier; t >= R_COMUN; t--) {
      int16_t cand[80];
      int n = 0;
      for (int16_t d = 1; d <= 151 && n < 80; d++) {
        if (DEX_TBL[d].rarity != t) continue;
        if (pass == 0 && !lineHasUnregistered(d)) continue;
        cand[n++] = d;
      }
      if (n > 0) return cand[random(n)];
    }
  }
  return CLASSIC_DEX[random(NUM_CLASSIC_DEX)];  // inalcanzable, por si acaso
}

void Pet::registerSpecies(int16_t dex) {
  if (dex < 1 || dex > 151) return;
  dexReg[(dex - 1) >> 3] |= (1 << ((dex - 1) & 7));
  if (shiny) dexShinyReg[(dex - 1) >> 3] |= (1 << ((dex - 1) & 7));
}

// la racha y el vinculo mejoran el sorteo del huevo (0..~14)
int Pet::careBonus() const {
  int s = streak > 30 ? 30 : streak;
  return s / 3 + bond / 25;
}

// primer cuidado del dia: avanza la racha y afianza el vinculo
void Pet::registerCare() {
  if (isEgg() || ceremony != CER_NONE) return;
  uint32_t d = today();
  if (d == 0 || d == lastCareDay) return;  // sin reloj, o ya conto hoy
  if (lastCareDay == 0 || d == lastCareDay + 1) {
    streak++;
  } else {
    streak = 1;        // hubo un hueco de dias
    lastMilestone = 0;
  }
  lastCareDay = d;
  bondToday = 0;
  if (streak > bestStreak) bestStreak = streak;
  bond = clamp100(bond + 4);
  uint16_t ms = (streak >= 100) ? 100 : (streak >= 30) ? 30
              : (streak >= 7)   ? 7   : (streak >= 3)  ? 3 : 0;
  if (ms > lastMilestone) {
    lastMilestone = ms;
    milestoneUntil = millis() + 4500;
  }
  checkMedals();
  save();
}

void Pet::addBond(uint8_t amt) {
  if (bondToday >= 20) return;  // tope diario: el vinculo no se farmea
  bond = clamp100(bond + amt);
  bondToday += amt;
}

void Pet::checkMedals() {
  if (isEgg()) return;
  uint16_t before = medals;
  if (level() >= 10) medals |= MED_LV10;
  if (level() >= 25) medals |= MED_LV25;
  if (level() >= 50) medals |= MED_LV50;
  if (berryKnown) medals |= MED_BERRY;
  if (streak >= 7) medals |= MED_STREAK7;
  if (bond >= 100) medals |= MED_BOND;
  if (DEX_TBL[speciesId].evolvesTo == 0) medals |= MED_FINAL;
  if (weight == 0 && level() >= 5 && careMistakes == 0) medals |= MED_FIT;
  uint16_t gained = medals & ~before;
  if (gained) {
    for (uint16_t m = gained; m; m &= (m - 1)) totalMedals++;
    newMedal = gained;
    medalUntil = millis() + 4000;
    if (!sleeping) sfxPlay(SFX_MEDAL);
    save();
  }
}

void Pet::rename(const char *name) {
  strncpy(nick, name, sizeof(nick) - 1);
  nick[sizeof(nick) - 1] = 0;
  save();
}

static uint16_t calcStat(uint8_t base, uint8_t gene, uint8_t lvl, uint8_t tr) {
  // 分段成长：等级越高，基础属性和训练转化效率都会提高，形成清晰的
  // 1/10/25/50/100 级晋升台阶，同时保留旧存档的基因和训练值。
  uint16_t tierBonus = lvl >= 100 ? 50 : lvl >= 50 ? 30 : lvl >= 25 ? 16 : lvl >= 10 ? 6 : 0;
  uint16_t levelGrowth = lvl + lvl / 5;
  uint16_t trainGrowth = tr + (uint16_t)tr * (lvl / 25) / 10;
  return (uint16_t)base * gene / 100 + levelGrowth + tierBonus + trainGrowth;
}

uint16_t Pet::atkStat() const {
  return isEgg() ? 0 : calcStat(DEX_TBL[speciesId].bAtk, geneAtk, level(), trAtk) + equipmentAtk;
}
uint16_t Pet::defStat() const {
  return isEgg() ? 0 : calcStat(DEX_TBL[speciesId].bDef, geneDef, level(), trDef) + equipmentDef;
}
uint16_t Pet::speStat() const {
  return isEgg() ? 0 : calcStat(DEX_TBL[speciesId].bSpe, geneSpe, level(), trSpe);
}

uint16_t Pet::registeredCount() const {
  uint16_t n = 0;
  for (int i = 1; i <= 151; i++)
    if (isRegistered(i)) n++;
  return n;
}

uint16_t Pet::caughtCount() const {
  uint16_t n = 0;
  for (int i = 1; i <= 151; i++)
    if (isCaught(i)) n++;
  return n;
}

uint16_t Pet::knownDexCount() const {
  uint16_t n = 0;
  for (int i = 1; i <= 151; i++)
    if (isRegistered(i) || isCaught(i)) n++;
  return n;
}

void Pet::registerCaught(int16_t dex) {
  if (dex < 1 || dex > 151) return;
  dexCaught[(dex - 1) >> 3] |= (1 << ((dex - 1) & 7));
  save();
}

uint8_t Pet::catchChanceForWild(int16_t wildDex, uint8_t wildLevel,
                                uint8_t petLevel, bool closeWin) const {
  if (wildDex < 1 || wildDex > DEX_COUNT || isCaught(wildDex)) return 0;
  const DexEntry &wild = DEX_TBL[wildDex];
  // Capture is intentionally a meaningful but achievable event: about 25%
  // at equal levels, with small changes for rarity, damage and bond.
  int chance = wild.rarity == R_RARO ? 16 : 25;
  int gap = (int)wildLevel - (int)(petLevel ? petLevel : 1);
  if (gap > 0) chance -= gap * 4;
  else if (gap < 0) chance += (-gap) * 2;
  if (closeWin) chance += 8;
  chance += bond / 25;
  if (wild.rarity == R_RARO && chance > 35) chance = 35;
  if (chance > 42) chance = 42;
  if (chance < 8) chance = 8;
  return (uint8_t)chance;
}

bool Pet::tryCatchWild(int16_t wildDex, uint8_t wildLevel, uint8_t petLevel,
                       bool closeWin, uint8_t luckRoll) {
  uint8_t chance = catchChanceForWild(wildDex, wildLevel, petLevel, closeWin);
  if (chance == 0 || (luckRoll % 100) >= chance) return false;
  registerCaught(wildDex);
  joy = clamp100(joy + 5);
  addBond(1);
  save();
  return true;
}

void Pet::applyBattleWin(int16_t wildDex, bool closeWin) {
  if (ceremony != CER_NONE || isEgg()) return;
  uint8_t gain = (wildDex >= 1 && DEX_TBL[wildDex].rarity == R_RARO) ? 2 : 1;
  if (closeWin) gain++;
  if (trAtk <= trDef && trAtk <= trSpe) trAtk = clamp100(trAtk + gain);
  else if (trDef <= trAtk && trDef <= trSpe) trDef = clamp100(trDef + gain);
  else trSpe = clamp100(trSpe + gain);
  battleWins++;
  battleStreak++;
  if (battleStreak > bestBattleStreak) bestBattleStreak = battleStreak;
  joy = clamp100(joy + 8 + (closeWin ? 4 : 0));
  energy = dropTo(energy, 8, 20);
  fullness = dropTo(fullness, 3, 10);
  addBond(closeWin ? 3 : 2);
  registerCare();
  earnCoins(2 + gain);
  save();
}

void Pet::applyBattleLoss() {
  if (ceremony != CER_NONE || isEgg()) return;
  battleLosses++;
  battleStreak = 0;
  joy = dropTo(joy, 12, 20);
  energy = dropTo(energy, 15, 20);
  fullness = dropTo(fullness, 4, 10);
  save();
}

// forma final que ya cumplio su ciclo (7 dias): lista para despedirse. La
// despedida la dispara el usuario con el boton (no salta sola, para que la vea)
bool Pet::canFarewellNow() const {
  return !isEgg() && !sleeping && ceremony == CER_NONE &&
         DEX_TBL[speciesId].evolvesTo == 0 && ageMinutes >= FAREWELL_AGE_MIN;
}

// abandono total durante 1h: lista para escaparse. La dispara el usuario con el
// boton (final triste); cuidarla un solo tick la salva (neglectTicks se resetea)
bool Pet::canRunawayNow() const {
  return !isEgg() && !sleeping && ceremony == CER_NONE && neglectTicks >= RUNAWAY_TICKS;
}

void Pet::startFarewell() {
  if (isEgg() || ceremony != CER_NONE) return;
  lastEnd = CER_FAREWELL;
  ceremony = CER_FAREWELL;
  ceremonyUntil = millis() + CEREMONY_MS;
  heartUntil = ceremonyUntil;  // corazones durante toda la despedida
  sfxPlay(SFX_BYE);
  save();
}

void Pet::startRunaway() {
  if (isEgg() || ceremony != CER_NONE) return;
  lastEnd = CER_RUNAWAY;
  ceremony = CER_RUNAWAY;
  ceremonyUntil = millis() + CEREMONY_MS;
  sfxPlay(SFX_BYE);
  save();
}

void Pet::release() {
  if (isEgg() || ceremony != CER_NONE) return;
  lastEnd = CER_RELEASE;
  ceremony = CER_RELEASE;
  ceremonyUntil = millis() + CEREMONY_MS;
  heartUntil = ceremonyUntil;
  sfxPlay(SFX_BYE);
  save();
}

void Pet::hatch() {
  speciesId = eggTarget;
  activeSpeciesId = speciesId;
  shiny = eggShiny;
  // genes del individuo: 90-110% por stat (cada crianza es unica)
  geneAtk = 90 + random(21);
  geneDef = 90 + random(21);
  geneSpe = 90 + random(21);
  trAtk = trDef = trSpe = 0;
  berryKnown = false;
  bond = 0;          // vinculo, medallas y nombre son del individuo
  bondToday = 0;
  medals = 0;
  newMedal = 0;
  nick[0] = 0;
  registerSpecies(speciesId);  // criado = registrado en la pokedex
  checkMedals();     // por si nace ya en forma final (legendario)
  sfxPlay(SFX_HATCH);
  save();
}

// ¿se dan ya las condiciones para evolucionar? Cada descuido retrasa la
// evolucion 1 nivel, y ademas tiene que estar bien cuidado en ese momento
// (ninguna estadistica por debajo de 40). NO evoluciona sola: la dispara el
// usuario tocando al bicho (evolve()), para que vea la transformacion.
bool Pet::canEvolveNow() const {
  if (isEgg() || sleeping || ceremony != CER_NONE) return false;
  const DexEntry &d = DEX_TBL[speciesId];
  if (d.evolvesTo == 0) return false;
  return level() >= (uint8_t)(d.evolveLevel + careMistakes) && lowestStat() >= 40;
}

void Pet::evolve() {
  if (!canEvolveNow()) return;
  const DexEntry &d = DEX_TBL[speciesId];
  prevSpeciesId = speciesId;
  int16_t next = d.evolvesTo;
  if (speciesId == DEX_EEVEE) {
    // rama de Eevee: prefiere la evolucion que falte en la pokedex
    int16_t opts[3];
    int n = 0;
    for (int16_t b = 134; b <= 136; b++)
      if (!isRegistered(b)) opts[n++] = b;
    next = n > 0 ? opts[random(n)] : (int16_t)(134 + random(3));
  }
  speciesId = next;
  registerSpecies(speciesId);
  sfxPlay(SFX_EVOLVE);
  evolveUntil = millis() + EVOLVE_ANIM_MS;
  save();
}

void Pet::feed() {
  feedBerry(0);
}

void Pet::earnCoins(uint16_t amount) {
  uint32_t next = (uint32_t)coins + amount;
  coins = next > 9999 ? 9999 : (uint16_t)next;
}

void Pet::feedBerry(uint8_t color) {
  if (ceremony != CER_NONE) return;
  if (isEgg() || sleeping) return;
  if (lovesBerry(color)) {
    fullness = clamp100(fullness + 35);
    joy = clamp100(joy + 10);
    heartUntil = millis() + HEART_MS;  // "le encanta!"
    berryKnown = true;                 // descubierto: se muestra en la ficha
    addBond(2);
  } else {
    fullness = clamp100(fullness + 25);
  }
  if (health < 100) health = clamp100(health + 2);
  eatUntil = millis() + EAT_ANIM_MS;
  registerCare();
  markDaily(1);
  save();
}

void Pet::feedCandy() {
  if (ceremony != CER_NONE) return;
  if (isEgg() || sleeping) return;
  fullness = clamp100(fullness + 10);
  joy = clamp100(joy + 12);
  if (health < 100) health = clamp100(health + 1);
  weight = clamp100(weight + 12);  // las chuches pasan factura
  eatUntil = millis() + EAT_ANIM_MS;
  registerCare();
  markDaily(1);
  save();
}

void Pet::playResult(uint8_t score) {
  if (ceremony != CER_NONE || isEgg()) return;
  uint8_t v = trSpe + score / 5;  // jugar entrena la velocidad
  trSpe = v > 100 ? 100 : v;
  joy = clamp100(joy + 5 + (score > 15 ? 30 : score * 2));
  energy = dropTo(energy, 10 + score / 2, 5);
  fullness = dropTo(fullness, 5, 5);
  int burn = (int)weight - score * 2;  // el ejercicio quema peso
  weight = burn > 0 ? burn : 0;
  if (score >= 5) heartUntil = millis() + HEART_MS;
  if (score > gameHi) gameHi = score;  // nuevo record
  earnCoins(3 + score / 4);
  addBond(2);
  registerCare();
  markDaily(4);
  save();
}

// TamaPetchi's memory game rewards a successful run while keeping the same
// care economy as the existing ball game: fun costs energy, and practice
// improves the pet instead of being a disconnected arcade screen.
void Pet::memoryResult(uint8_t score, bool won) {
  if (ceremony != CER_NONE || isEgg()) return;
  if (score > memoryHi) memoryHi = score;
  uint8_t joyGain = won ? (uint8_t)(12 + score / 3) : (uint8_t)(4 + score / 6);
  joy = clamp100(joy + joyGain);
  energy = dropTo(energy, won ? 8 : 5, 5);
  fullness = dropTo(fullness, won ? 4 : 2, 5);
  if (won) health = clamp100(health + 5);
  earnCoins(2 + score);
  addBond(won ? 3 : 1);
  registerCare();
  markDaily(4);
  save();
}

// saco de entrenamiento: los golpes entrenan la fuerza. Devuelve la subida.
uint8_t Pet::trainStrength(uint16_t hits) {
  if (ceremony != CER_NONE || isEgg()) return 0;
  uint8_t gain = hits / 4;          // ~4 golpes = 1 punto de entrenamiento
  if (gain > 18) gain = 18;         // tope por sesion: la FUE se forja a fuego lento
  uint8_t v = trAtk + gain;
  trAtk = v > 100 ? 100 : v;
  energy = dropTo(energy, 12, 5);   // cansa
  fullness = dropTo(fullness, 5, 5);
  int burn = (int)weight - hits / 3;  // tambien quema peso
  weight = burn > 0 ? burn : 0;
  joy = clamp100(joy + 6);
  if (hits >= 20) heartUntil = millis() + HEART_MS;
  if (hits > strHi) strHi = hits;   // record de golpes
  earnCoins(1 + hits / 12);
  addBond(2);
  registerCare();
  markDaily(4);
  save();
  return gain;
}

bool Pet::buyItem(uint8_t item) {
  if (ceremony != CER_NONE || isEgg() || sleeping || item >= SHOP_ITEM_COUNT) return false;
  // Do not charge for a decoration purchase when every available toy is owned.
  if ((item == SHOP_TOY || item == SHOP_TOY_BOX) && decorOwned == 0xFF) return false;
  static const uint16_t COST[SHOP_ITEM_COUNT] = { 3, 3, 3, 4, 8, 12, 14, 18 };
  if (coins < COST[item]) return false;
  coins -= COST[item];
  switch (item) {
    case SHOP_BERRY_RED:
    case SHOP_BERRY_BLUE:
    case SHOP_BERRY_GREEN: {
      uint8_t color = item == SHOP_BERRY_RED ? 0 : item == SHOP_BERRY_BLUE ? 1 : 2;
      if (lovesBerry(color)) {
        fullness = clamp100(fullness + 35);
        joy = clamp100(joy + 10);
        berryKnown = true;
        addBond(2);
        heartUntil = millis() + HEART_MS;
      } else {
        fullness = clamp100(fullness + 25);
      }
      if (health < 100) health = clamp100(health + 2);
      eatUntil = millis() + EAT_ANIM_MS;
      break;
    }
    case SHOP_CANDY:
      fullness = clamp100(fullness + 10);
      joy = clamp100(joy + 12);
      weight = clamp100(weight + 12);
      eatUntil = millis() + EAT_ANIM_MS;
      break;
    case SHOP_TOY: {
      // Cada compra de juguete desbloquea el siguiente objeto de decoracion.
      for (uint16_t bit = 1; bit <= 128; bit <<= 1) {
        if (!(decorOwned & bit)) {
          decorOwned |= bit;
          decorPlaced[room < 4 ? room : 0] |= bit;
          break;
        }
      }
      joy = clamp100(joy + 25);
      energy = dropTo(energy, 3, 5);
      break;
    }
    case SHOP_MEDICINE:
      health = clamp100(health + 30);
      hygiene = clamp100(hygiene + 8);
      break;
    case SHOP_TRAIN_TOKEN:
      trAtk = clamp100(trAtk + 2);
      joy = clamp100(joy + 6);
      energy = dropTo(energy, 4, 5);
      break;
    case SHOP_TOY_BOX: {
      // 玩具箱一次解锁两个尚未拥有的装饰，仍然遵守 8 个玩具上限。
      uint8_t unlocked = 0;
      for (uint16_t bit = 1; bit <= 128 && unlocked < 2; bit <<= 1) {
        if (!(decorOwned & bit)) {
          decorOwned |= bit;
          decorPlaced[room < 4 ? room : 0] |= bit;
          unlocked++;
        }
      }
      joy = clamp100(joy + 35);
      energy = dropTo(energy, 4, 5);
      break;
    }
  }
  registerCare();
  save();
  return true;
}

bool Pet::buyShopProduct(uint8_t category, uint8_t slot) {
  if (ceremony != CER_NONE || isEgg() || sleeping || category >= SHOP_CAT_COUNT || slot >= SHOP_ITEMS_PER_CATEGORY)
    return false;
  const ShopProduct &product = SHOP_PRODUCTS[category][slot];
  if (coins < product.price) return false;
  coins -= product.price;
  uint8_t id = shopProductId(category, slot);
  if (warehouse[id] < 250) warehouse[id]++;
  registerCare();
  save();
  return true;
}

static uint8_t equipmentSlotForItem(uint8_t slot) {
  if (slot == 2) return EQUIP_HELMET;
  if (slot == 3 || slot == 8 || slot == 17) return EQUIP_ARMOR;
  if (slot == 5) return EQUIP_SHOES;
  // 武器/护手/宝石按左右手分配；同一手再次装备会替换旧件。
  return (slot & 1) ? EQUIP_RIGHT_HAND : EQUIP_LEFT_HAND;
}

static void applyEquipmentBonus(uint8_t slot, int sign,
                                uint8_t &atk, uint8_t &def, uint8_t &imm) {
  if (slot >= SHOP_ITEMS_PER_CATEGORY) return;
  int value = (int)SHOP_PRODUCTS[SHOP_CAT_EQUIPMENT][slot].value * sign;
  if (slot == 0 || slot == 6 || slot == 7 || slot == 10 || slot == 12 || slot == 15 || slot == 19)
    atk = (uint8_t)constrain((int)atk + value, 0, 100);
  else if (slot == 1 || slot == 2 || slot == 3 || slot == 8 || slot == 11 || slot == 16 || slot == 17)
    def = (uint8_t)constrain((int)def + value, 0, 100);
  else
    imm = (uint8_t)constrain((int)imm + value, 0, 100);
}

bool Pet::equipShopProduct(uint8_t slot) {
  if (ceremony != CER_NONE || isEgg() || sleeping || slot >= SHOP_ITEMS_PER_CATEGORY) return false;
  uint8_t id = shopProductId(SHOP_CAT_EQUIPMENT, slot);
  if (!warehouse[id]) return false;
  uint8_t target = equipmentSlotForItem(slot);
  uint8_t old = equipped[target];
  if (old != EQUIP_EMPTY) {
    uint8_t oldId = shopProductId(SHOP_CAT_EQUIPMENT, old);
    if (warehouse[oldId] < 250) warehouse[oldId]++;
    applyEquipmentBonus(old, -1, equipmentAtk, equipmentDef, equipmentImm);
  }
  warehouse[id]--;
  equipped[target] = slot;
  applyEquipmentBonus(slot, +1, equipmentAtk, equipmentDef, equipmentImm);
  registerCare();
  save();
  return true;
}

bool Pet::unequipSlot(uint8_t equipSlot) {
  if (ceremony != CER_NONE || isEgg() || sleeping || equipSlot >= EQUIP_SLOT_COUNT) return false;
  uint8_t slot = equipped[equipSlot];
  if (slot == EQUIP_EMPTY) return false;
  uint8_t id = shopProductId(SHOP_CAT_EQUIPMENT, slot);
  if (warehouse[id] < 250) warehouse[id]++;
  applyEquipmentBonus(slot, -1, equipmentAtk, equipmentDef, equipmentImm);
  equipped[equipSlot] = EQUIP_EMPTY;
  save();
  return true;
}

bool Pet::sellShopProduct(uint8_t category, uint8_t slot) {
  if (ceremony != CER_NONE || isEgg() || sleeping || category >= SHOP_CAT_COUNT || slot >= SHOP_ITEMS_PER_CATEGORY)
    return false;
  uint8_t id = shopProductId(category, slot);
  if (!warehouse[id]) return false;
  if (category == SHOP_CAT_EQUIPMENT) {
    for (uint8_t i = 0; i < EQUIP_SLOT_COUNT; ++i) {
      if (equipped[i] == slot) unequipSlot(i);
    }
  }
  warehouse[id]--;
  uint16_t refund = SHOP_PRODUCTS[category][slot].price / 2;
  earnCoins(refund ? refund : 1);
  save();
  return true;
}

bool Pet::useShopProduct(uint8_t category, uint8_t slot) {
  if (ceremony != CER_NONE || isEgg() || sleeping || category >= SHOP_CAT_COUNT || slot >= SHOP_ITEMS_PER_CATEGORY)
    return false;
  uint8_t id = shopProductId(category, slot);
  bool systemFood = category == SHOP_CAT_FOOD && slot < 4;
  if (!warehouse[id] && !systemFood) return false;
  const ShopProduct &product = SHOP_PRODUCTS[category][slot];
  if (category == SHOP_CAT_TRAVEL) {
    // One completed fuel batch (30 cleaned poops) powers one trip.
    if (fuelBatches == 0) return false;
    fuelBatches--;
    room = (uint8_t)(slot % 4);
    joy = clamp100(joy + 12 + product.value);
  } else if (category == SHOP_CAT_FOOD) {
    fullness = clamp100(fullness + product.value);
    joy = clamp100(joy + (product.value / 3));
    if (slot >= 5) weight = clamp100(weight + 4);
    eatUntil = millis() + EAT_ANIM_MS;
  } else if (category == SHOP_CAT_TOY) {
    joy = clamp100(joy + product.value);
    energy = dropTo(energy, 3, 5);
    heartUntil = millis() + HEART_MS;
  } else if (category == SHOP_CAT_MEDICINE) {
    // Different remedies address the symptoms shown on the status screen.
    if (slot == 1 || slot == 8) hygiene = clamp100(hygiene + product.value);
    else if (slot == 6) energy = clamp100(energy + product.value);
    else health = clamp100(health + product.value);
  } else if (category == SHOP_CAT_EQUIPMENT) {
    if (slot == 0 || slot == 6 || slot == 7) equipmentAtk = clamp100(equipmentAtk + product.value);
    if (slot == 1 || slot == 2 || slot == 3 || slot == 8) equipmentDef = clamp100(equipmentDef + product.value);
    if (slot == 4 || slot == 5 || slot == 9) equipmentImm = clamp100(equipmentImm + product.value);
  } else if (category == SHOP_CAT_PROP) {
    propOwned |= (uint16_t)(1u << slot);
    if (slot < 8) {
      decorOwned |= (uint8_t)(1u << slot);
      decorPlaced[room < 4 ? room : 0] |= (uint8_t)(1u << slot);
    }
  }
  if (warehouse[id]) warehouse[id]--;
  registerCare();
  save();
  return true;
}

bool Pet::toggleProp(uint8_t slot) {
  if (ceremony != CER_NONE || isEgg() || sleeping || slot >= 10) return false;
  uint16_t bit = (uint16_t)(1u << slot);
  if (!(propOwned & bit)) return false;
  propPlaced[room < 4 ? room : 0] ^= bit;
  save();
  return true;
}

bool Pet::toggleDecor(uint8_t slot) {
  if (ceremony != CER_NONE || isEgg() || sleeping || slot > 7) return false;
  uint8_t bit = (uint8_t)(1u << slot);
  if (!(decorOwned & bit)) return false;
  uint8_t r = room < 4 ? room : 0;
  decorPlaced[r] ^= bit;
  save();
  return true;
}

bool Pet::visitRoom(uint8_t nextRoom) {
  if (ceremony != CER_NONE || isEgg() || sleeping || nextRoom > 3) return false;
  if (room == nextRoom) return true;
  room = nextRoom;
  if (nextRoom != 0) joy = clamp100(joy + 8);
  registerCare();
  save();
  return true;
}

void Pet::play() {
  if (ceremony != CER_NONE) return;
  if (isEgg() || sleeping) return;
  joy = clamp100(joy + 25);
  energy = clamp100(energy - 10);
  fullness = clamp100(fullness - 5);
  heartUntil = millis() + HEART_MS;
  addBond(2);
  registerCare();
  save();
}

void Pet::toggleLight() {
  if (ceremony != CER_NONE) return;
  if (isEgg()) return;
  sleeping = !sleeping;
  save();
}

void Pet::clean() {
  if (ceremony != CER_NONE) return;
  // La limpieza recoge las cacas actuales y las convierte en combustible
  // persistente. El combustible se conserva aunque la barra de cacas vuelva a cero.
  uint16_t collected = (uint16_t)poopFuel + poops;
  if (collected >= 30) {
    uint16_t add = collected / 30;
    fuelBatches = fuelBatches > (uint16_t)(65535 - add) ? 65535 : (uint16_t)(fuelBatches + add);
  }
  poopFuel = (uint8_t)(collected % 30);
  poops = 0;
  hygiene = 100;
  health = clamp100(health + 8);
  addBond(1);
  registerCare();
  markDaily(2);
  save();
}

void Pet::caress() {
  if (ceremony != CER_NONE) return;
  if (isEgg() || sleeping) return;
  joy = clamp100(joy + 5);
  heartUntil = millis() + HEART_MS;
  addBond(1);
  registerCare();
}

void Pet::eggTap() {
  if (!isEgg()) return;
  if (++eggTaps >= 3) hatch();
  else save();
}

PetMood Pet::mood() const {
  if (sleeping) return MOOD_SLEEPING;
  if (eating()) return MOOD_EATING;
  if (health < 25 || lowestStat() < 25) return MOOD_SAD;
  return MOOD_HAPPY;
}

// A compact per-species snapshot makes the Pokedex selection real without
// allocating a large 151-entry RAM table. Each selected creature keeps its
// own care state in NVS; non-selected creatures therefore stop aging.
struct PetSlotSnapshot {
  uint32_t ageMinutes;
  uint8_t fullness, joy, energy, hygiene, health, poops, poopFuel, weight;
  uint8_t geneAtk, geneDef, geneSpe, trAtk, trDef, trSpe;
  uint8_t berryKnown, shiny, careMistakes, sleeping, bond;
  uint16_t medals;
  uint8_t equipmentAtk, equipmentDef, equipmentImm;
  uint8_t equipped[EQUIP_SLOT_COUNT];
  char nick[12];
};

// 当前发布前已存在的快照格式：有便便燃料，但还没有独立装备槽。
struct PetSlotSnapshotV1 {
  uint32_t ageMinutes;
  uint8_t fullness, joy, energy, hygiene, health, poops, poopFuel, weight;
  uint8_t geneAtk, geneDef, geneSpe, trAtk, trDef, trSpe;
  uint8_t berryKnown, shiny, careMistakes, sleeping, bond;
  uint16_t medals;
  char nick[12];
};

// Snapshot layout used before persistent poop fuel was introduced. Keeping a
// reader for it prevents an existing creature from being reset on upgrade.
struct PetSlotSnapshotLegacy {
  uint32_t ageMinutes;
  uint8_t fullness, joy, energy, hygiene, health, poops, weight;
  uint8_t geneAtk, geneDef, geneSpe, trAtk, trDef, trSpe;
  uint8_t berryKnown, shiny, careMistakes, sleeping, bond;
  uint16_t medals;
  char nick[12];
};

static void petSlotKey(int16_t dex, char *key, size_t n) {
  snprintf(key, n, "slot%03d", dex);
}

uint8_t Pet::speciesPoops(int16_t dex) {
  if (dex == speciesId) return poopFuel;
  if (dex < 1 || dex > 151) return 0;
  PetSlotSnapshot slot = {};
  char key[12];
  petSlotKey(dex, key, sizeof(key));
  if (prefs.getBytes(key, &slot, sizeof(slot)) == sizeof(slot)) return slot.poopFuel;
  PetSlotSnapshotV1 old = {};
  if (prefs.getBytes(key, &old, sizeof(old)) == sizeof(old)) return old.poopFuel;
  return 0;
}

uint16_t Pet::speciesFuelBatches(int16_t dex) {
  if (dex == speciesId) return fuelBatches;
  if (dex < 1 || dex > 151) return 0;
  char key[12];
  snprintf(key, sizeof(key), "fuelN%03d", dex);
  return prefs.getUShort(key, 0);
}

void Pet::saveActiveSlot() {
  if (speciesId < 1 || speciesId > 151) return;
  PetSlotSnapshot slot = {
    ageMinutes, fullness, joy, energy, hygiene, health, poops, poopFuel, weight,
    geneAtk, geneDef, geneSpe, trAtk, trDef, trSpe,
    (uint8_t)berryKnown, (uint8_t)shiny, careMistakes, (uint8_t)sleeping, bond,
    medals, equipmentAtk, equipmentDef, equipmentImm,
    { equipped[0], equipped[1], equipped[2], equipped[3], equipped[4] }, ""
  };
  strncpy(slot.nick, nick, sizeof(slot.nick) - 1);
  char key[12];
  petSlotKey(speciesId, key, sizeof(key));
  prefs.putBytes(key, &slot, sizeof(slot));
  char fuelKey[12];
  snprintf(fuelKey, sizeof(fuelKey), "fuelN%03d", speciesId);
  prefs.putUShort(fuelKey, fuelBatches);
}

bool Pet::loadActiveSlot(int16_t dex) {
  PetSlotSnapshot slot = {};
  char key[12];
  petSlotKey(dex, key, sizeof(key));
  bool current = prefs.getBytes(key, &slot, sizeof(slot)) == sizeof(slot);
  if (current) {
    ageMinutes = slot.ageMinutes;
    fullness = slot.fullness; joy = slot.joy; energy = slot.energy;
    hygiene = slot.hygiene; health = slot.health; poops = slot.poops;
    poopFuel = slot.poopFuel > 30 ? 30 : slot.poopFuel;
    char fuelKey[12];
    snprintf(fuelKey, sizeof(fuelKey), "fuelN%03d", dex);
    fuelBatches = prefs.getUShort(fuelKey, 0);
    weight = slot.weight;
    geneAtk = slot.geneAtk; geneDef = slot.geneDef; geneSpe = slot.geneSpe;
    trAtk = slot.trAtk; trDef = slot.trDef; trSpe = slot.trSpe;
    berryKnown = slot.berryKnown; shiny = slot.shiny;
    careMistakes = slot.careMistakes; sleeping = slot.sleeping; bond = slot.bond;
    medals = slot.medals;
    equipmentAtk = slot.equipmentAtk;
    equipmentDef = slot.equipmentDef;
    equipmentImm = slot.equipmentImm;
    for (uint8_t i = 0; i < EQUIP_SLOT_COUNT; i++) equipped[i] = slot.equipped[i];
    strncpy(nick, slot.nick, sizeof(nick) - 1); nick[sizeof(nick) - 1] = 0;
    return true;
  }
  PetSlotSnapshotV1 prior = {};
  if (prefs.getBytes(key, &prior, sizeof(prior)) == sizeof(prior)) {
    ageMinutes = prior.ageMinutes;
    fullness = prior.fullness; joy = prior.joy; energy = prior.energy;
    hygiene = prior.hygiene; health = prior.health; poops = prior.poops;
    poopFuel = prior.poopFuel > 30 ? 30 : prior.poopFuel;
    char fuelKey[12];
    snprintf(fuelKey, sizeof(fuelKey), "fuelN%03d", dex);
    fuelBatches = prefs.getUShort(fuelKey, 0);
    weight = prior.weight;
    geneAtk = prior.geneAtk; geneDef = prior.geneDef; geneSpe = prior.geneSpe;
    trAtk = prior.trAtk; trDef = prior.trDef; trSpe = prior.trSpe;
    berryKnown = prior.berryKnown; shiny = prior.shiny;
    careMistakes = prior.careMistakes; sleeping = prior.sleeping; bond = prior.bond;
    medals = prior.medals;
    equipmentAtk = equipmentDef = equipmentImm = 0;
    for (uint8_t i = 0; i < EQUIP_SLOT_COUNT; i++) equipped[i] = EQUIP_EMPTY;
    strncpy(nick, prior.nick, sizeof(nick) - 1); nick[sizeof(nick) - 1] = 0;
    return true;
  }
  PetSlotSnapshotLegacy old = {};
  if (prefs.getBytes(key, &old, sizeof(old)) != sizeof(old)) return false;
  ageMinutes = old.ageMinutes;
  fullness = old.fullness; joy = old.joy; energy = old.energy;
  hygiene = old.hygiene; health = old.health; poops = old.poops;
  poopFuel = 0; fuelBatches = 0; weight = old.weight;
  geneAtk = old.geneAtk; geneDef = old.geneDef; geneSpe = old.geneSpe;
  trAtk = old.trAtk; trDef = old.trDef; trSpe = old.trSpe;
  berryKnown = old.berryKnown; shiny = old.shiny;
  careMistakes = old.careMistakes; sleeping = old.sleeping; bond = old.bond;
  medals = old.medals;
  equipmentAtk = equipmentDef = equipmentImm = 0;
  for (uint8_t i = 0; i < EQUIP_SLOT_COUNT; i++) equipped[i] = EQUIP_EMPTY;
  strncpy(nick, old.nick, sizeof(nick) - 1); nick[sizeof(nick) - 1] = 0;
  return true;
}

bool Pet::selectSpecies(int16_t dex) {
  if (dex < 1 || dex > 151 || ceremony != CER_NONE || isEgg()) return false;
  if (!isRegistered(dex) && !isCaught(dex)) return false;
  if (speciesId == dex) { activeSpeciesId = dex; save(); return true; }
  saveActiveSlot();
  speciesId = dex;
  activeSpeciesId = dex;
  if (!loadActiveSlot(dex)) {
    ageMinutes = 0; fullness = joy = energy = 80; hygiene = health = 100;
    poops = poopFuel = weight = 0; fuelBatches = 0; geneAtk = geneDef = geneSpe = 100;
    trAtk = trDef = trSpe = 0; careMistakes = 0; bond = 0; medals = 0;
    berryKnown = false; shiny = isShinyRegistered(dex); sleeping = false; nick[0] = 0;
  }
  registerSpecies(dex);
  save();
  return true;
}

void Pet::save() {
  ticksSinceSave = 0;
  pendingSave = false;
  prefs.putUChar("full", fullness);
  prefs.putUChar("joy", joy);
  prefs.putUChar("ene", energy);
  prefs.putUChar("hyg", hygiene);
  prefs.putUChar("hp", health);
  prefs.putUChar("poop", poops);
  prefs.putUChar("fuel", poopFuel);
  prefs.putUShort("fuelN", fuelBatches);
  prefs.putUChar("wgt", weight);
  prefs.putUShort("coin", coins);
  prefs.putUChar("room", room);
  prefs.putUChar("dOwn", decorOwned);
  prefs.putBytes("dRoom", decorPlaced, sizeof(decorPlaced));
  prefs.putBytes("stock", warehouse, sizeof(warehouse));
  prefs.putUShort("pOwn", propOwned);
  prefs.putBytes("pRoom", propPlaced, sizeof(propPlaced));
  prefs.putUChar("eqAtk", equipmentAtk);
  prefs.putUChar("eqDef", equipmentDef);
  prefs.putUChar("eqImm", equipmentImm);
  prefs.putBytes("eqSlots", equipped, sizeof(equipped));
  prefs.putUChar("gatk", geneAtk);
  prefs.putUChar("gdef", geneDef);
  prefs.putUChar("gspe", geneSpe);
  prefs.putUChar("tatk", trAtk);
  prefs.putUChar("tdef", trDef);
  prefs.putUChar("tspe", trSpe);
  prefs.putBool("bk", berryKnown);
  prefs.putBool("shy", shiny);
  prefs.putBool("eshy", eggShiny);
  prefs.putBool("stpk", starterPick);
  prefs.putBytes("dexsh", dexShinyReg, sizeof(dexShinyReg));
  prefs.putBytes("dexcgt", dexCaught, sizeof(dexCaught));
  prefs.putUInt("age", ageMinutes);
  prefs.putShort("dexn", speciesId);
  prefs.putShort("eggT2", eggTarget);
  prefs.putUChar("crack", eggTaps);
  prefs.putUChar("mist", careMistakes);
  prefs.putBool("sleep", sleeping);
  prefs.putUChar("lend", lastEnd);
  if (lastSeenEpoch) prefs.putUInt("seen", lastSeenEpoch);
  prefs.putBytes("dexreg", dexReg, sizeof(dexReg));
  prefs.putUShort("strk", streak);
  prefs.putUShort("bstrk", bestStreak);
  prefs.putUInt("cday", lastCareDay);
  prefs.putUChar("bond", bond);
  prefs.putUShort("medal", medals);
  prefs.putUShort("tmedal", totalMedals);
  prefs.putUShort("mstone", lastMilestone);
  prefs.putUShort("ghi", gameHi);
  prefs.putUShort("mhi", memoryHi);
  prefs.putUShort("shi", strHi);
  prefs.putUShort("bwin", battleWins);
  prefs.putUShort("bloss", battleLosses);
  prefs.putUShort("bstk", battleStreak);
  prefs.putUShort("bbstk", bestBattleStreak);
  prefs.putShort("active", activeSpeciesId);
  prefs.putUChar("daily", dailyFlags);
  prefs.putString("nick", nick);
  saveActiveSlot();
}

void Pet::load() {
  fullness = prefs.getUChar("full", 80);
  joy = prefs.getUChar("joy", 80);
  energy = prefs.getUChar("ene", 80);
  hygiene = prefs.getUChar("hyg", 100);
  health = prefs.getUChar("hp", 100);
  poops = prefs.getUChar("poop", 0);
  fuelBatches = prefs.getUShort("fuelN", 0);
  poopFuel = prefs.getUChar("fuel", 0);
  if (poopFuel >= 30) {
    fuelBatches = (uint16_t)(fuelBatches + poopFuel / 30);
    poopFuel = (uint8_t)(poopFuel % 30);
  }
  weight = prefs.getUChar("wgt", 0);
  coins = prefs.getUShort("coin", 20);
  room = prefs.getUChar("room", 0);
  if (room > 3) room = 0;
  decorOwned = prefs.getUChar("dOwn", 1);
  if (!decorOwned) decorOwned = 1;
  size_t dlen = prefs.getBytes("dRoom", decorPlaced, sizeof(decorPlaced));
  if (dlen != sizeof(decorPlaced)) {
    decorPlaced[0] = 1;
    decorPlaced[1] = decorPlaced[2] = decorPlaced[3] = 0;
  }
  memset(warehouse, 0, sizeof(warehouse));
  prefs.getBytes("stock", warehouse, sizeof(warehouse));
  propOwned = prefs.getUShort("pOwn", 0);
  memset(propPlaced, 0, sizeof(propPlaced));
  prefs.getBytes("pRoom", propPlaced, sizeof(propPlaced));
  equipmentAtk = prefs.getUChar("eqAtk", 0);
  equipmentDef = prefs.getUChar("eqDef", 0);
  equipmentImm = prefs.getUChar("eqImm", 0);
  for (uint8_t i = 0; i < EQUIP_SLOT_COUNT; i++) equipped[i] = EQUIP_EMPTY;
  if (prefs.getBytes("eqSlots", equipped, sizeof(equipped)) != sizeof(equipped)) {
    for (uint8_t i = 0; i < EQUIP_SLOT_COUNT; i++) equipped[i] = EQUIP_EMPTY;
  }
  geneAtk = prefs.getUChar("gatk", 0);
  geneDef = prefs.getUChar("gdef", 0);
  geneSpe = prefs.getUChar("gspe", 0);
  if (geneAtk == 0) {  // mascota anterior a los genes: tirada unica ahora
    geneAtk = 90 + random(21);
    geneDef = 90 + random(21);
    geneSpe = 90 + random(21);
  }
  trAtk = prefs.getUChar("tatk", 0);
  trDef = prefs.getUChar("tdef", 0);
  trSpe = prefs.getUChar("tspe", 0);
  berryKnown = prefs.getBool("bk", false);
  shiny = prefs.getBool("shy", false);
  eggShiny = prefs.getBool("eshy", false);
  starterPick = prefs.getBool("stpk", false);
  prefs.getBytes("dexsh", dexShinyReg, sizeof(dexShinyReg));
  prefs.getBytes("dexcgt", dexCaught, sizeof(dexCaught));
  ageMinutes = prefs.getUInt("age", 0);
  if (prefs.isKey("dexn")) {
    speciesId = prefs.getShort("dexn", -1);
    eggTarget = prefs.getShort("eggT2", 4);
  } else {
    // migracion desde la version con indices de flash (0-8)
    static const uint8_t OLD2DEX[9] = { 4, 5, 6, 1, 2, 3, 7, 8, 9 };
    int8_t old = prefs.getChar("spec", -1);
    speciesId = (old >= 0 && old < 9) ? OLD2DEX[old] : -1;
    int8_t oldT = prefs.getChar("eggT", 0);
    eggTarget = (oldT >= 0 && oldT < 9) ? OLD2DEX[oldT] : 4;
  }
  eggTaps = prefs.getUChar("crack", 0);
  careMistakes = prefs.getUChar("mist", 0);
  sleeping = prefs.getBool("sleep", false);
  lastEnd = prefs.getUChar("lend", CER_NONE);
  prefs.getBytes("dexreg", dexReg, sizeof(dexReg));
  streak = prefs.getUShort("strk", 0);
  bestStreak = prefs.getUShort("bstrk", 0);
  lastCareDay = prefs.getUInt("cday", 0);
  bond = prefs.getUChar("bond", 0);
  medals = prefs.getUShort("medal", 0);
  totalMedals = prefs.getUShort("tmedal", 0);
  lastMilestone = prefs.getUShort("mstone", 0);
  gameHi = prefs.getUShort("ghi", 0);
  memoryHi = prefs.getUShort("mhi", 0);
  strHi = prefs.getUShort("shi", 0);
  battleWins = prefs.getUShort("bwin", 0);
  battleLosses = prefs.getUShort("bloss", 0);
  battleStreak = prefs.getUShort("bstk", 0);
  bestBattleStreak = prefs.getUShort("bbstk", 0);
  activeSpeciesId = prefs.getShort("active", speciesId);
  dailyFlags = prefs.getUChar("daily", 0);
  if (activeSpeciesId < 1 || activeSpeciesId > 151) activeSpeciesId = speciesId;
  prefs.getString("nick", nick, sizeof(nick));
  // siembra: la mascota actual cuenta como criada (guardados antiguos)
  if (speciesId >= 1) registerSpecies(speciesId);
}
