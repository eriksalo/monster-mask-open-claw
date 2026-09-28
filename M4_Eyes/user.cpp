// SPDX-FileCopyrightText: 2024 Monster Mask Open Claw Project
//
// SPDX-License-Identifier: MIT

// Auto-cycling eye style controller for Monster M4SK.
// Every 2 minutes, advances to the next eye style and reboots.
// Also triggers on nose boop or covering the light sensor.
// Cycle state persists across soft resets via SAMD51 RTC backup registers
// (resets to style 0 on power cycle).
//
// Serial commands at 115200 baud:
//   MOOD:<name>     Switch to named eye style immediately
//   MOOD:list       List available eye styles
//   MOOD:next       Skip to next style immediately
//   STATUS          Print current style and frame info
//   AUTOCYCLE:on    Enable auto-cycling (default)
//   AUTOCYCLE:off   Disable auto-cycling

#if 1 // Change to 0 to disable this code (must enable ONE user*.cpp only!)

#include "globals.h"
#include <string.h>

extern uint32_t frames;  // Defined in M4_Eyes.ino
extern bool     booped;  // Defined in M4_Eyes.ino
extern Adafruit_Arcada arcada; // Defined via globals.h GLOBAL_VAR

// Eye style name to config path mapping
struct StyleEntry {
  const char *name;
  const char *configPath;
};

static const StyleEntry styleTable[] = {
  { "hazel",       "hazel/config.eye"       },
  { "anime",       "anime/config.eye"       },
  { "big_blue",    "big_blue/config.eye"    },
  { "demon",       "demon/config.eye"       },
  { "doom_red",    "doom-red/config.eye"    },
  { "doom_spiral", "doom-spiral/config.eye" },
  { "fish",        "fish_eyes/config.eye"   },
  { "fizzgig",     "fizzgig/config.eye"     },
  { "hypno_red",   "hypno_red/config.eye"   },
  { "reflection",  "reflection/config.eye"  },
  { "skull",       "skull/config.eye"       },
  { "snake",       "snake_green/config.eye" },
  { "spikes",      "spikes/config.eye"      },
  { "toonstripe",  "toonstripe/config.eye"  },
};
static const uint8_t NUM_STYLES = sizeof(styleTable) / sizeof(styleTable[0]);

// Persistent state across soft resets using SAMD51 RTC backup registers.
// BKUP[0] holds magic (upper 16 bits) + style index (lower 8 bits).
// BKUP[1] holds magic + autocycle enabled flag.
// These survive NVIC_SystemReset but clear on power-on reset (POR).
#define CYCLE_MAGIC_MASK 0xC7C10000
static uint8_t cycleIndex;
static uint8_t cycleEnabled;

static void loadCycleState(void) {
  uint32_t v0 = RTC->MODE0.BKUP[0].reg;
  uint32_t v1 = RTC->MODE0.BKUP[1].reg;
  if ((v0 & 0xFFFF0000) == CYCLE_MAGIC_MASK) {
    cycleIndex = v0 & 0xFF;
    if (cycleIndex >= NUM_STYLES) cycleIndex = 0;
  } else {
    cycleIndex = 0;
  }
  if ((v1 & 0xFFFF0000) == CYCLE_MAGIC_MASK) {
    cycleEnabled = v1 & 0xFF;
  } else {
    cycleEnabled = 1; // Default: on
  }
}

static void saveCycleState(void) {
  RTC->MODE0.BKUP[0].reg = CYCLE_MAGIC_MASK | cycleIndex;
  RTC->MODE0.BKUP[1].reg = CYCLE_MAGIC_MASK | cycleEnabled;
}

// Auto-cycle timer
static unsigned long lastCycleMs  = 0;
static const unsigned long CYCLE_MS = 120000; // 2 minutes

// Sensor trigger state
static unsigned long bootMs        = 0;      // millis() at boot, skip sensors briefly
static const unsigned long SENSOR_GRACE_MS = 5000; // Ignore sensors for 5s after boot

// Nose boop detection (rising edge of 'booped' global)
static bool prevBooped = false;

// Light sensor cover detection
static unsigned long lastLightMs   = 0;
static const unsigned long LIGHT_POLL_MS = 200; // Poll seesaw every 200ms
static const uint16_t LIGHT_COVER_THRESH = 50;  // Below this = covered (0-1023 range)
static uint8_t darkCount           = 0;          // Consecutive dark readings
static bool    wasDark             = false;       // Already triggered on this cover

// Serial input buffer
static char    serialBuf[64];
static uint8_t serialIdx = 0;

// Called from M4_Eyes.ino setup() BEFORE loadConfig to get the right config path.
const char *getCycleConfigPath(void) {
  loadCycleState();
  return styleTable[cycleIndex].configPath;
}

// Reboot into a specific style index
static void rebootToStyle(uint8_t idx) {
  cycleIndex = idx % NUM_STYLES;
  saveCycleState();
  Serial.printf("STYLE:REBOOTING:%s\n", styleTable[cycleIndex].name);
  Serial.flush();
  delay(50);
  NVIC_SystemReset();
}

// Process a complete command line
static void processCommand(const char *cmd) {
  while (*cmd == ' ' || *cmd == '\t') cmd++;

  if (!strncasecmp(cmd, "MOOD:", 5)) {
    const char *name = cmd + 5;

    if (!strcasecmp(name, "list")) {
      Serial.println("STYLE:LIST");
      for (uint8_t i = 0; i < NUM_STYLES; i++) {
        Serial.printf("  %s -> %s%s\n", styleTable[i].name,
                       styleTable[i].configPath,
                       (i == cycleIndex) ? " [current]" : "");
      }
      Serial.printf("STYLE:CURRENT:%s\n", styleTable[cycleIndex].name);
      Serial.printf("AUTOCYCLE:%s\n", cycleEnabled ? "on" : "off");
      return;
    }

    if (!strcasecmp(name, "next")) {
      rebootToStyle(cycleIndex + 1);
      return; // Won't reach here
    }

    for (uint8_t i = 0; i < NUM_STYLES; i++) {
      if (!strcasecmp(name, styleTable[i].name)) {
        rebootToStyle(i);
        return; // Won't reach here
      }
    }
    Serial.printf("UNKNOWN:STYLE:%s\n", name);

  } else if (!strncasecmp(cmd, "AUTOCYCLE:", 9)) {
    const char *val = cmd + 9;
    if (!strcasecmp(val, "on")) {
      cycleEnabled = 1;
      saveCycleState();
      lastCycleMs = millis();
      Serial.println("AUTOCYCLE:on");
    } else if (!strcasecmp(val, "off")) {
      cycleEnabled = 0;
      saveCycleState();
      Serial.println("AUTOCYCLE:off");
    }

  } else if (!strncasecmp(cmd, "STATUS", 6)) {
    Serial.printf("STATUS:style=%s,index=%d/%d,autocycle=%s,frames=%lu,freeRAM=%lu\n",
                  styleTable[cycleIndex].name, cycleIndex, NUM_STYLES,
                  cycleEnabled ? "on" : "off",
                  (unsigned long)frames, (unsigned long)availableRAM());

  } else if (cmd[0] != '\0') {
    Serial.printf("UNKNOWN:CMD:%s\n", cmd);
  }
}

void user_setup(void) {
  Serial.printf("Eye style: %s (%d/%d) autocycle=%s\n",
                styleTable[cycleIndex].name, cycleIndex, NUM_STYLES,
                cycleEnabled ? "on (2 min)" : "off");
  Serial.println("Triggers: nose boop, cover light sensor, 2-min timer, serial");
  bootMs = millis();
  lastCycleMs = bootMs;
}

void user_loop(void) {
  unsigned long now = millis();
  bool sensorsReady = (now - bootMs >= SENSOR_GRACE_MS);

  // During grace period, track sensor baselines so we don't false-trigger
  // immediately after grace ends (e.g., sensor already covered at boot).
  if (!sensorsReady) {
    prevBooped = booped;
    if (now - lastLightMs >= LIGHT_POLL_MS) {
      lastLightMs = now;
      uint16_t reading = arcada.readLightSensor();
      if (reading <= 1023 && reading < LIGHT_COVER_THRESH) {
        wasDark = true; // Already dark at boot, don't trigger
      }
    }
  }

  // --- Nose boop: rising edge triggers next style ---
  if (sensorsReady && booped && !prevBooped) {
    Serial.println("TRIGGER:boop");
    rebootToStyle(cycleIndex + 1);
  }
  prevBooped = booped;

  // --- Light sensor: covering triggers next style ---
  if (sensorsReady && (now - lastLightMs >= LIGHT_POLL_MS)) {
    lastLightMs = now;
    uint16_t reading = arcada.readLightSensor();
    if (reading <= 1023) { // Valid I2C read
      if (reading < LIGHT_COVER_THRESH) {
        if (darkCount < 255) darkCount++;
        if (!wasDark && darkCount >= 3) { // 3 consecutive dark readings
          wasDark = true;
          Serial.println("TRIGGER:light_cover");
          rebootToStyle(cycleIndex + 1);
        }
      } else {
        darkCount = 0;
        wasDark = false;
      }
    }
  }

  // --- Auto-cycle timer: reboot into next style ---
  if (cycleEnabled && (now - lastCycleMs >= CYCLE_MS)) {
    Serial.println("TRIGGER:timer");
    rebootToStyle(cycleIndex + 1);
  }

  // Non-blocking serial read
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialIdx > 0) {
        serialBuf[serialIdx] = '\0';
        processCommand(serialBuf);
        serialIdx = 0;
      }
    } else if (serialIdx < sizeof(serialBuf) - 1) {
      serialBuf[serialIdx++] = c;
    }
  }
}

#endif // 1
