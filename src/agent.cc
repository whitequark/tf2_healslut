// We're trying to get an object file built against glibc to link against musl, which is naughty.
// It does work though if hardening is disabled.
#undef _FORTIFY_SOURCE

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <time.h>
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

// start source sdk junk
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-local-typedefs"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wreorder"
#pragma GCC diagnostic ignored "-Wclass-memaccess"
#pragma GCC diagnostic ignored "-Wignored-qualifiers"
#pragma GCC diagnostic ignored "-Wextra"

#include <algorithm>
using std::swap;

#define POSIX   1
#define LINUX   1
#define _LINUX  1
#define GNUC    1
#define USE_SDL 1
#define CLIENT_DLL    1
#define TF_CLIENT_DLL 1
#define NO_MALLOC_OVERRIDE  1
#include "basehandle.h"
#include "icliententity.h"
#include "icliententitylist.h"
#include "client_class.h"
#include "cdll_int.h"

#pragma GCC diagnostic pop
// end source sdk junk

const char *tf_class_name[] = { NULL,
  "scout",   "sniper", "soldier",
  "demoman", "medic",  "heavy",
  "pyro",    "spy",    "engineer",
};

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

void dump_props(RecvTable *table, int depth) {
  for (int i = 0; i < depth; i++) log("  ");
  log("+ %s\n", table->GetName());
  int max_number = 0;
  for (int index = 0; index < table->GetNumProps(); index++) {
    RecvProp *prop = table->GetProp(index);
    if (isdigit(prop->GetName()[0])) {
      max_number = max(max_number, strtol(prop->GetName(), NULL, 10));
      if (max_number > 0) continue;
    }
    for (int i = 0; i < depth + 1; i++) log("  ");
    log("- %s\n", prop->GetName());
    if (prop->GetType() == DPT_DataTable)
      dump_props(prop->GetDataTable(), depth + 2);
  }
  if (max_number > 0) {
    for (int i = 0; i < depth + 1; i++) log("  ");
    log("- %03d..%03d\n", 1, max_number);
  }
}

void dump_class_props(ClientClass *classes) {
  for (ClientClass *klass = classes; klass; klass = klass->m_pNext) {
    log("* %s\n", klass->GetName());
    dump_props(klass->m_pRecvTable, 1);
  }
}

int get_prop_offset(RecvTable *) {
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

int open_socket(struct addrinfo *host_ai) {
  int connect_errno = 0;
  for (struct addrinfo *ai_it = host_ai; ai_it; ai_it = ai_it->ai_next) {
    int it_sock = socket(ai_it->ai_family, ai_it->ai_socktype, ai_it->ai_protocol);
    if (it_sock == -1)
      die("[!] cannot open socket: %s\n", strerror(errno));

    if (connect(it_sock, ai_it->ai_addr, ai_it->ai_addrlen) == 0)
      return it_sock;
    connect_errno = errno;

    close(it_sock);
  }
  die("[!] cannot connect to socket: %s\n", strerror(connect_errno));
}

void write_http_post(int sock, const char *host, const char *path, const char *body) {
  char buffer[1024], *header = buffer;
  size_t body_len = strlen(body);
  size_t header_len = snprintf(buffer, sizeof(buffer),
    "POST %s HTTP/1.1\r\n"
    "Host: %s\r\n"
    "User-Agent: tf2_healslut\r\n"
    "Content-Type: application/x-www-form-urlencoded\r\n"
    "Content-Length: %zd\r\n"
    "\r\n",
    path, host, body_len);
  if (header_len >= sizeof(buffer))
    die("[!] HTTP request header is too large (%zd bytes)\n", header_len);

  while (header_len > 0) {
    ssize_t written = write(sock, header, header_len);
    if (written == -1)
      die("[!] cannot write HTTP request header: %s\n", strerror(errno));
    header += written;
    header_len -= written;
  }

  while (body_len > 0) {
    ssize_t written = write(sock, body, body_len);
    if (written == -1)
      die("[!] cannot write HTTP request body: %s\n", strerror(errno));
    body += written;
    body_len -= written;
  }
}

bool resolve_influxdb(struct addrinfo **ai, const char **host, const char **path) {
  char *influxdb = getenv("INFLUXDB");
  if (influxdb == NULL) return false;
  influxdb = strdup(influxdb);

  char *host_sep = strchr(influxdb, ',');
  char *port_sep = NULL;
  if (host_sep != NULL)
    port_sep = strchr(host_sep + 1, ',');
  if (host_sep == NULL || port_sep == NULL)
    die("[!] invalid INFLUXDB='%s' syntax: must be 'HOST,[PORT],PATH'\n", influxdb);

  *host = influxdb;
  *host_sep = 0;
  const char *port = (host_sep + 1 == port_sep) ? "8086" : host_sep + 1;
  *port_sep = 0;
  *path = port_sep + 1;

  struct addrinfo hints = {};
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICSERV;

  int gai_result = getaddrinfo(*host, port, &hints, ai);
  if (gai_result == EAI_SYSTEM)
    die("[!] cannot resolve '%s:%s': %s\n", *host, port, strerror(errno));
  else if (gai_result != 0)
    die("[!] cannot resolve '%s:%s': %s\n", *host, port, gai_strerror(gai_result));

  return true;
}

void write_influxdb(struct addrinfo *ai, const char *host, const char *path,
                    const char *format, ...) {
  char buffer[1024];
  va_list va;
  va_start(va, format);
  size_t length = vsnprintf(buffer, sizeof(buffer), format, va);
  va_end(va);
  if (length >= sizeof(buffer))
    die("[!] influxdb batch is too large (%zd bytes)\n", length);

  int sock = open_socket(ai);
  write_http_post(sock, host, path, buffer);
  close(sock);
}

int agent() {
  setvbuf(stdout, NULL, _IONBF, 0);

  struct addrinfo *influx_ai;
  const char *influx_host, *influx_path;
  bool use_influxdb = resolve_influxdb(&influx_ai, &influx_host, &influx_path);

  IBaseClientDLL *VClient017 =
    (IBaseClientDLL *)get_interface("client.so", "VClient017");
  IClientEntityList *VClientEntityList003 =
    (IClientEntityList *)get_interface("client.so", "VClientEntityList003");
  IVEngineClient013 *VEngineClient014 =
    (IVEngineClient013 *)get_interface("engine.so", "VEngineClient014");

  ClientClass *all_classes = VClient017->GetAllClasses();
  if (getenv("DUMP_PROPS")) {
    dump_class_props(all_classes);
    return 0;
  }

  int offset_m_iPrimaryAmmoType = get_class_prop_offset(all_classes,
    "DT_BaseCombatWeapon", "LocalWeaponData", "m_iPrimaryAmmoType");
  int offset_m_iClip1 = get_class_prop_offset(all_classes,
    "DT_BaseCombatWeapon", "LocalWeaponData", "m_iClip1");
  int offset_m_flEnergy = get_class_prop_offset(all_classes,
    "DT_TFWeaponBase", "m_flEnergy");
  int offset_m_hHealingTarget = get_class_prop_offset(all_classes,
    "DT_WeaponMedigun", "m_hHealingTarget");
  int offset_m_flChargeLevel = get_class_prop_offset(all_classes,
    "DT_WeaponMedigun", "LocalTFWeaponMedigunData", "m_flChargeLevel");
  int offset_m_bHealing = get_class_prop_offset(all_classes,
    "DT_WeaponMedigun", "m_bHealing");
  int offset_m_bChargeRelease = get_class_prop_offset(all_classes,
    "DT_WeaponMedigun", "m_bChargeRelease");
  int offset_m_hActiveWeapon = get_class_prop_offset(all_classes,
    "DT_BaseCombatCharacter", "m_hActiveWeapon");
  int offset_m_hMyWeapons = get_class_prop_offset(all_classes,
    "DT_BaseCombatCharacter", "m_hMyWeapons");
  int offset_m_iAmmo = get_class_prop_offset(all_classes,
    "DT_BasePlayer", "localdata", "m_iAmmo");
  int offset_m_nPlayerCond = get_class_prop_offset(all_classes,
    "DT_TFPlayer", "m_Shared", "m_nPlayerCond");
  int offset_m_nNumHealers = get_class_prop_offset(all_classes,
    "DT_TFPlayer", "m_Shared", "m_nNumHealers");
  int offset_m_iDisguiseTargetIndex = get_class_prop_offset(all_classes,
    "DT_TFPlayer", "m_Shared", "m_iDisguiseTargetIndex");
  int offset_m_nDisguiseClass = get_class_prop_offset(all_classes,
    "DT_TFPlayer", "m_Shared", "m_nDisguiseClass");
  int offset_m_iDisguiseHealth = get_class_prop_offset(all_classes,
    "DT_TFPlayer", "m_Shared", "m_iDisguiseHealth");
  int offset_m_iPlayerClass = get_class_prop_offset(all_classes,
    "DT_TFPlayerResource", "m_iPlayerClass");
  int offset_m_bAlive = get_class_prop_offset(all_classes,
    "DT_TFPlayerResource", "baseclass", "m_bAlive");
  int offset_m_iHealth = get_class_prop_offset(all_classes,
    "DT_TFPlayerResource", "baseclass", "m_iHealth");
  int offset_m_iTotalScore = get_class_prop_offset(all_classes,
    "DT_TFPlayerResource", "m_iTotalScore");
  int offset_m_iScore = get_class_prop_offset(all_classes,
    "DT_TFPlayerResource", "baseclass", "m_iScore");
  int offset_m_iStreaks = get_class_prop_offset(all_classes,
    "DT_TFPlayerResource", "m_iStreaks"); // int[3+1][MAX_PLAYERS+1]
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

  while (1) {
    IClientEntity *player_resource = NULL;
    int highest_entity_index = VClientEntityList003->GetHighestEntityIndex();
    for (int entity_index = 0; entity_index < highest_entity_index; entity_index++) {
      IClientEntity *entity = VClientEntityList003->GetClientEntity(entity_index);
      if (entity == NULL) continue;
      if (!strcmp(entity->GetClientClass()->GetName(), "CTFPlayerResource")) {
        player_resource = entity;
        break;
      }
    }
    if (player_resource == NULL) {
      log("[=] no game in progress\n");
    } else {
      log("[=] found CTFPlayerResource entity at " WPRIxPTR "\n",
        (uintptr_t)player_resource);

      int player_index = VEngineClient014->GetLocalPlayer();
      IClientEntity *player = VClientEntityList003->GetClientEntity(player_index);
      log("[=] local player index is %d\n", player_index);

      int player_class_index =
        get_entity_prop<int>(player_resource, offset_m_iPlayerClass, player_index);
      const char *player_class = tf_class_name[player_class_index];

      CBaseHandle active_weapon_handle =
        get_entity_prop<CBaseHandle>(player, offset_m_hActiveWeapon);
      IClientEntity *active_weapon =
        VClientEntityList003->GetClientEntityFromHandle(active_weapon_handle);
      const char *active_weapon_type = NULL;
      int active_weapon_clip = -1, active_weapon_ammo = -1, active_weapon_energy = -1;
      if (active_weapon != NULL) {
        active_weapon_type = active_weapon->GetClientClass()->GetName();
        active_weapon_clip = get_entity_prop<int>(active_weapon, offset_m_iClip1);
        int ammo_index =
          get_entity_prop<int>(active_weapon, offset_m_iPrimaryAmmoType);
        if (ammo_index != -1)
          active_weapon_ammo = get_entity_prop<int>(player, offset_m_iAmmo, ammo_index);
        active_weapon_energy = get_entity_prop<float>(active_weapon, offset_m_flEnergy);
      }

      IClientEntity *medigun = NULL;
      for (int weapon_index = 0; weapon_index < 48; weapon_index++) {
        CBaseHandle active_weapon_handle =
          get_entity_prop<CBaseHandle>(player, offset_m_hMyWeapons, weapon_index);
        if (!active_weapon_handle.IsValid()) continue;
        IClientEntity *weapon =
          VClientEntityList003->GetClientEntityFromHandle(active_weapon_handle);
        if (!strcmp(weapon->GetClientClass()->GetName(), "CWeaponMedigun"))
          medigun = weapon;
      }

      int ubercharge = -1;
      bool is_healing = false, is_ubering = false;
      if (medigun != NULL) {
        ubercharge = get_entity_prop<float>(medigun, offset_m_flChargeLevel) * 100;
        is_healing = get_entity_prop<bool>(medigun, offset_m_bHealing);
        is_ubering = get_entity_prop<bool>(medigun, offset_m_bChargeRelease);
      }

      int score = get_entity_prop<int>(player_resource, offset_m_iTotalScore, player_index);
      int kills = get_entity_prop<int>(player_resource, offset_m_iScore, player_index);
      int killstreak =
        get_entity_prop<int>(player_resource, offset_m_iStreaks, player_index * 4 + 0);
      bool alive = get_entity_prop<bool>(player_resource, offset_m_bAlive, player_index);
      int health = get_entity_prop<int>(player_resource, offset_m_iHealth, player_index);
      int max_health = get_entity_prop<int>(player_resource, offset_m_iMaxHealth, player_index);
      int max_buffed_health =
        get_entity_prop<int>(player_resource, offset_m_iMaxBuffedHealth, player_index);
      int damage = get_entity_prop<int>(player_resource, offset_m_iDamage, player_index);
      int damage_assist =
        get_entity_prop<int>(player_resource, offset_m_iDamageAssist, player_index);
      int healing = get_entity_prop<int>(player_resource, offset_m_iHealing, player_index);
      int healing_assist =
        get_entity_prop<int>(player_resource, offset_m_iHealingAssist, player_index);

      IClientEntity *patient = NULL;
      int patient_class_index = 0;
      const char *patient_class = NULL;
      int patient_health = -1, patient_max_health = -1, patient_max_buffed_health = -1;
      int patient_num_healers = -1;
      if (medigun != NULL) {
        CBaseHandle patient_handle =
          get_entity_prop<CBaseHandle>(medigun, offset_m_hHealingTarget);
        patient = VClientEntityList003->GetClientEntityFromHandle(patient_handle);
        if (patient != NULL) {
          patient_num_healers = get_entity_prop<int>(patient, offset_m_nNumHealers);
          int patient_index;
          if (get_entity_prop<int>(patient, offset_m_nPlayerCond) & (1 << 3)) {
            patient_index = get_entity_prop<int>(patient, offset_m_iDisguiseTargetIndex);
            patient_health = get_entity_prop<int>(patient, offset_m_iDisguiseHealth);
            patient_class_index = get_entity_prop<int>(patient, offset_m_nDisguiseClass);
            patient = VClientEntityList003->GetClientEntity(patient_index);
          } else {
            patient_index = patient_handle.GetEntryIndex();
            patient_health = get_entity_prop<int>(player_resource, offset_m_iHealth, player_index);
            patient_class_index =
              get_entity_prop<int>(player_resource, offset_m_iPlayerClass, patient_index);
          }
          patient_class = tf_class_name[patient_class_index];
          patient_max_health =
            get_entity_prop<int>(player_resource, offset_m_iMaxHealth, patient_index);
          patient_max_buffed_health =
            get_entity_prop<int>(player_resource, offset_m_iMaxBuffedHealth, patient_index);
        }
      }

      log("[@] player class is %s\n", player_class);
      log("[@] player weapon is %s (ammo %d/%d, energy %d)\n",
        active_weapon_type, active_weapon_clip, active_weapon_ammo, active_weapon_energy);
      if (medigun != NULL) {
        log("[@] player ubercharge is %d\n", ubercharge);
        log("[@] player is %s\n",
          is_ubering ? "releasing charge" : is_healing ? "healing" : "not healing");
      }
      log("[@] player score is %d (%d kills), killstreak %d\n", score, kills, killstreak);
      log("[@] player is %s\n", alive ? "alive" : "dead");
      log("[@] player health is %d/%d(%d)\n", health, max_health, max_buffed_health);
      log("[@] player damage is %d(%d)\n", damage, damage_assist);
      log("[@] player healing is %d(%d)\n", healing, healing_assist);

      if (patient != NULL) {
        log("[@] patient class is %s\n", patient_class);
        log("[@] patient health is %d/%d(%d)\n",
          patient_health, patient_max_health, patient_max_buffed_health);
        log("[@] patient has %d healers\n", patient_num_healers);
      }

      if (use_influxdb) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t timestamp = (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;

        const char *tags = getenv("INFLUX_TAGS");
        if (tags == NULL) tags = "";
        char tag_sep = *tags ? ',' : ' ';

        write_influxdb(influx_ai, influx_host, influx_path,
          "player%c%s "
            "class=\"%s\",max_health=%di,max_buffed_health=%di,"
            "weapon=\"%s\",clip=%di,ammo=%di,energy=%di,"
            "ubercharge=%di,is_healing=%c,is_ubering=%c,"
            "score=%di,kills=%di,killstreak=%di,alive=%c,"
            "health=%di,damage=%di,damage_assist=%di,healing=%di,healing_assist=%di "
            "%" PRId64,
          tag_sep, tags,
          player_class ?: "", max_health, max_buffed_health,
          active_weapon_type ?: "", active_weapon_clip, active_weapon_ammo, active_weapon_energy,
          ubercharge, is_healing ? 't' : 'f', is_ubering ? 't' : 'f',
          score, kills, killstreak, alive ? 't' : 'f',
          health, damage, damage_assist, healing, healing_assist,
          timestamp);

        if (patient != NULL) {
          write_influxdb(influx_ai, influx_host, influx_path,
            "patient%c%s "
              "class=\"%s\",max_health=%di,max_buffed_health=%di,"
              "health=%di,num_healers=%di "
              "%" PRId64,
            tag_sep, tags,
            patient_class ?: "", patient_max_health, patient_max_buffed_health,
            patient_health, patient_num_healers,
            timestamp);
        }
      }
    }

    sleep(1);
    flush_process();
  }

  return 0;
}
