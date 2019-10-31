#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include "unfork.hh"

static void log(const char *format, ...) __attribute__((format (printf, 1, 2)));
static void die(const char *format, ...) __attribute__((format (printf, 1, 2), noreturn));

static void log(const char *format, ...) {
  va_list va;
  va_start(va, format);
  vfprintf(stdout, format, va);
  va_end(va);
}

static void die(const char *format, ...) {
  va_list va;
  va_start(va, format);
  vfprintf(stderr, format, va);
  va_end(va);
  _Exit(1);
}

#if UINTPTR_MAX > 0xffffffff
#define WPRIxPTR "%016" PRIxPTR
#define WPRIxSZ  "%016zx"
#else
#define WPRIxPTR "%08" PRIxPTR
#define WPRIxSZ  "%08zx"
#endif

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-local-typedefs"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wreorder"
#pragma GCC diagnostic ignored "-Wclass-memaccess"
#pragma GCC diagnostic ignored "-Wignored-qualifiers"
#pragma GCC diagnostic ignored "-Wextra"

// start source sdk junk
#include <algorithm>
using std::swap;

#define POSIX   1
#define LINUX   1
#define _LINUX  1
#define GNUC    1
#define USE_SDL 1
#include "basehandle.h"
#include "icliententity.h"
#include "icliententitylist.h"
#include "client_class.h"
#include "cdll_int.h"
// end source sdk junk

#pragma GCC diagnostic pop

void *get_interface(const char *shlib_name, const char *iface_name) {
  log("[=] requesting interface %s from %s\n", iface_name, shlib_name);
  void *(*CreateInterface)(const char *, int *) =
    (void *(*)(const char *, int *))get_symbol(shlib_name, "CreateInterface");
  void *iface = CreateInterface(iface_name, nullptr);
  if (iface == NULL)
    die("[!] no such interface\n");
  log("[=] got interface at " WPRIxPTR "\n", (uintptr_t)iface);
  return iface;
}

int get_prop_offset(RecvTable *table) {
  return 0;
}

template<typename ...Args>
int get_prop_offset(RecvTable *table, const char *name, Args&&... args) {
  for (int index = 0; index < table->GetNumProps(); index++) {
    RecvProp *prop = table->GetProp(index);
    if (!strcmp(prop->GetName(), name))
      return prop->GetOffset() + get_prop_offset(prop->GetDataTable(), args...);
  }
  die("[!] property '%s' not found\n", name);
}

template<typename ...Args>
int get_class_prop_offset(ClientClass *classes, const char *name, Args&&... args) {
  for (ClientClass *klass = classes; klass; klass = klass->m_pNext) {
    if (!strcmp(klass->m_pRecvTable->GetName(), name)) {
      int offset = get_prop_offset(klass->m_pRecvTable, args...);
      log("[=] found %s", name);
      for (const char *subname : {args...}) log(".%s", subname);
      log(" at offset %d\n", offset);
      return offset;
    }
  }
  die("[!] class '%s' not found\n", name);
}

template<typename PropT>
inline PropT get_entity_prop(IClientEntity *entity, int offset, int index = 0) {
  return *(PropT *)((uintptr_t)entity + offset + sizeof(PropT) * index);
}

int agent() {
  setvbuf(stdout, NULL, _IONBF, 0);

  IBaseClientDLL *VClient017 =
    (IBaseClientDLL *)get_interface("client.so", "VClient017");
  IClientEntityList *VClientEntityList003 =
    (IClientEntityList *)get_interface("client.so", "VClientEntityList003");
  IVEngineClient013 *VEngineClient014 =
    (IVEngineClient013 *)get_interface("engine.so", "VEngineClient014");

  ClientClass *all_classes = VClient017->GetAllClasses();

  int offset_m_iScore = get_class_prop_offset(all_classes,
    "DT_TFPlayerResource", "baseclass", "m_iScore");
  int offset_m_iTotalScore = get_class_prop_offset(all_classes,
    "DT_TFPlayerResource", "m_iTotalScore");
  int offset_m_iHealth = get_class_prop_offset(all_classes,
    "DT_TFPlayerResource", "baseclass", "m_iHealth");
  int offset_m_iMaxHealth = get_class_prop_offset(all_classes,
    "DT_TFPlayerResource", "m_iMaxHealth");
  int offset_m_iMaxBuffedHealth = get_class_prop_offset(all_classes,
    "DT_TFPlayerResource", "m_iMaxBuffedHealth");;
  int offset_m_iDamage = get_class_prop_offset(all_classes,
    "DT_TFPlayerResource", "m_iDamage");
  int offset_m_iDamageAssist = get_class_prop_offset(all_classes,
    "DT_TFPlayerResource", "m_iDamageAssist");
  int offset_m_iHealing = get_class_prop_offset(all_classes,
    "DT_TFPlayerResource", "m_iHealing");
  int offset_m_iHealingAssist = get_class_prop_offset(all_classes,
    "DT_TFPlayerResource", "m_iHealingAssist");
  int offset_m_hActiveWeapon = get_class_prop_offset(all_classes,
    "DT_BaseCombatCharacter", "m_hActiveWeapon");

  IClientEntity *tf_player_resource = NULL;
  int highest_entity_index = VClientEntityList003->GetHighestEntityIndex();
  for (int entity_index = 0; entity_index < highest_entity_index; entity_index++) {
    IClientEntity *entity = VClientEntityList003->GetClientEntity(entity_index);
    if (entity == NULL) continue;
    if (!strcmp(entity->GetClientClass()->GetName(), "CTFPlayerResource")) {
      tf_player_resource = entity;
      break;
    }
  }
  if (tf_player_resource == NULL)
    die("[!] cannot find CTFPlayerResource entity\n");
  log("[=] found CTFPlayerResource entity at " WPRIxPTR "\n",
    (uintptr_t)tf_player_resource);

  int local_player_index = VEngineClient014->GetLocalPlayer();
  IClientEntity *local_player = VClientEntityList003->GetClientEntity(local_player_index);
  log("[=] local player index is %d\n", local_player_index);

  while (1) {
    log("[@] local player score is %d (%d kills)\n",
      get_entity_prop<int>(tf_player_resource, offset_m_iScore, local_player_index),
      get_entity_prop<int>(tf_player_resource, offset_m_iTotalScore, local_player_index));

    log("[@] local player score is %d (%d kills)\n",
      get_entity_prop<int>(tf_player_resource, offset_m_iScore, local_player_index),
      get_entity_prop<int>(tf_player_resource, offset_m_iTotalScore, local_player_index));

    log("[@] local player health is %d/%d(%d)\n",
      get_entity_prop<int>(tf_player_resource, offset_m_iHealth, local_player_index),
      get_entity_prop<int>(tf_player_resource, offset_m_iMaxHealth, local_player_index),
      get_entity_prop<int>(tf_player_resource, offset_m_iMaxBuffedHealth, local_player_index));
    log("[@] local player damage is %d(%d)\n",
      get_entity_prop<int>(tf_player_resource, offset_m_iDamage, local_player_index),
      get_entity_prop<int>(tf_player_resource, offset_m_iDamageAssist, local_player_index));
    log("[@] local player healing is %d(%d)\n",
      get_entity_prop<int>(tf_player_resource, offset_m_iHealing, local_player_index),
      get_entity_prop<int>(tf_player_resource, offset_m_iHealingAssist, local_player_index));

    CBaseHandle active_weapon_handle =
      get_entity_prop<CBaseHandle>(local_player, offset_m_hActiveWeapon);
    IClientEntity *active_weapon =
      VClientEntityList003->GetClientEntityFromHandle(active_weapon_handle);
    log("[@] local player weapon class is %s\n", active_weapon->GetClientClass()->GetName());

    sleep(1);
    flush_process();
  }

  return 0;
}
