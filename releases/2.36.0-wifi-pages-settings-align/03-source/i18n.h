#pragma once
#include <Arduino.h>

// Idiomas soportados. ZH usa el canvas UTF-8 integrado con glifos 25x25; los demas idiomas
// conservan el render ASCII compacto original.
enum Lang : uint8_t { LANG_ES = 0, LANG_EN, LANG_FR, LANG_DE, LANG_IT, LANG_PT, LANG_ZH, LANG_COUNT };
#define LANG_DEFAULT LANG_ZH  // idioma por defecto: chino simplificado

extern Lang gLang;  // idioma activo (definido en i18n.cpp)

// IDs de cadena. El orden debe coincidir con la tabla STRINGS de i18n.cpp.
enum StrId : uint8_t {
  // estado del bicho (statusMsg)
  S_EVOLVING, S_EATING, S_LIKES, S_HUNGRY, S_NEEDS_BATH,
  S_EXHAUSTED, S_SAD, S_CHUBBY, S_IS_SHINY, S_HAPPY,
  // ceremonias de despedida
  S_FAREWELL, S_RUNAWAY, S_GOODBYE,
  // huevo
  S_EGG_HDR, S_EGG_LEGEND, S_EGG_RARE, S_EGG_TOUCH, S_EGG_MOVES, S_EGG_ALMOST,
  // formatos compartidos
  S_POKEDEX_FMT,   // "POKEDEX %u/151"
  S_NAME_FMT,      // "%s%s Nv.%u"
  // dialogo soltar
  S_RELEASE_FMT, S_YES, S_NO,
  // minijuego y saco
  S_HITS_FMT, S_STR_GAIN_FMT, S_NEW_RECORD, S_RECORD_FMT, S_HIT_FAST,
  S_SCORE_FMT, S_GREAT_JOY, S_PLUS_JOY,
  // reloj / ajustes
  S_SET_TIME, S_HOUR, S_MIN, S_CLOCK_CANCEL, S_LANG_LABEL,
  // celebracion
  S_MEDAL_BANNER, S_GREAT, S_STREAK_DAYS_FMT,
  // ficha: perfil
  S_STREAK_FMT, S_VIN, S_BERRY_UNK, S_BERRY_RED, S_BERRY_BLUE, S_BERRY_GREEN,
  S_INFO_FMT, S_RENAME_HINT,
  // ficha: combate
  S_BATTLE, S_STAT_ATK, S_STAT_DEF, S_STAT_SPE, S_STAT_WGT, S_TRAIN_STR,
  // ficha: medallas
  S_MEDALS_FMT, S_BACK,
  // teclado y galeria
  S_NAME, S_DETAIL_BACK,
  // barras
  S_BAR_FOOD, S_BAR_JOY, S_BAR_ENE, S_BAR_HYG,
  // marcador en vivo del minijuego
  S_REC_FMT,
  // ficha: pagina de progreso
  S_PROGRESS, S_LVL_FMT, S_NEXT_LVL_FMT, S_EVO_LABEL, S_FINAL_FORM,
  S_EVO_READY, S_EVO_BLOCKED, S_EVO_IN_FMT, S_MISTAKES_FMT,
  // interruptor de sonido (ajustes)
  S_SND_ON, S_SND_OFF,
  S_EVO_TAP,        // texto del boton de evolucion
  S_FAREWELL_BTN,   // texto del boton de despedida (lleva el nombre: "%s ...")
  S_RUNAWAY_BTN,    // texto del boton de escapada por abandono (final triste)
  // dialogos de decision (evolucionar/mantener, despedirse/quedaros)
  S_EVO_Q, S_EVO_KEEP, S_FAR_Q, S_FAR_GO, S_FAR_STAY,
  S_CHOOSE_STARTER,  // titulo de la eleccion del inicial (primera vez)
  S_NO_SPRITES, S_LOAD_SPRITES,  // aviso cuando falta el sprite en la SD
  // funciones移植adas de TamaPetchi
  S_GAME_MENU, S_GAME_BALL, S_GAME_MEMORY,
  S_MEM_WATCH, S_MEM_TURN, S_MEM_WIN, S_MEM_FAIL,
  S_STAT_HP,
  S_WORLD, S_SHOP, S_HOME, S_PARK, S_BEACH, S_FOREST,
  S_COINS_FMT, S_MEAL, S_TOY, S_MEDICINE, S_TICKET,
  S_BOUGHT, S_NOT_ENOUGH, S_COST_FMT,
  S_DECORATE, S_BALL, S_FLOWERS, S_TENT, S_LAMP, S_TAP_DECOR, S_PLACED,
  S_DRUM, S_BLOCKS, S_TRAIN, S_KITE,
  S_GALLERY_SHOP_HINT,
  S_CANDY_SHOP, S_TRAIN_TOKEN_SHOP, S_TOY_BOX_SHOP, S_SHOP_FOOD_HINT,
  // 野外战斗与捕捉
  S_BATTLE_WILD, S_BATTLE_ATTACK, S_BATTLE_DODGE, S_BATTLE_REST,
  S_BATTLE_QUICK, S_BATTLE_HEAVY, S_BATTLE_WIN, S_BATTLE_LOSS,
  S_BATTLE_HIT_FMT, S_BATTLE_REST_FMT, S_BATTLE_CATCH, S_BATTLE_LEAVE,
  S_BATTLE_CAUGHT, S_BATTLE_ESCAPED, S_BATTLE_BACK, S_BATTLE_ROUND_FMT,
  S_BATTLE_DAMAGE_FMT, S_BATTLE_EFFECTIVE, S_BATTLE_WEAK, S_BATTLE_RUN,
  STR_COUNT
};

const char *T(StrId id);       // texto en el idioma activo
const char *medalName(int i);  // banner de medalla (MED_COUNT)
const char *medalLabel(int i); // etiqueta corta de medalla
const char *medalDesc(int i);  // descripcion larga de medalla

void loadLang();             // lee el idioma de NVS (llamar en setup)
void setLang(Lang l);        // cambia y persiste el idioma
