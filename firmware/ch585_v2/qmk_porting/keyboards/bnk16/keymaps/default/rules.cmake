# BNK16 feature switches: ON compiles the feature in. Hardware settings are in ../../rules.cmake.

# Wireless: both stay OFF. The port integrates the WCH BLE stack directly, not through these QMK frameworks.
set(BLE_ENABLE OFF CACHE BOOL "KB" FORCE)
set(ESB_ENABLE OFF CACHE BOOL "KB" FORCE)

set(EXTRAKEY_ENABLE ON CACHE BOOL "KB" FORCE)        # media and system keys

# Configurable on Vial's own tabs
set(TAP_DANCE_ENABLE ON CACHE BOOL "KB" FORCE)
set(COMBO_ENABLE ON CACHE BOOL "KB" FORCE)
set(KEY_OVERRIDE_ENABLE ON CACHE BOOL "KB" FORCE)
set(REPEAT_KEY_ENABLE ON CACHE BOOL "KB" FORCE)

# QMK features used through their keycodes
set(ONESHOT_ENABLE ON CACHE BOOL "KB" FORCE)
set(SPACECADET_ENABLE ON CACHE BOOL "KB" FORCE)
set(CAPS_WORD_ENABLE ON CACHE BOOL "KB" FORCE)
set(LEADER_ENABLE OFF CACHE BOOL "KB" FORCE)
set(AUTO_SHIFT_ENABLE OFF CACHE BOOL "KB" FORCE)
set(SENTENCE_CASE_ENABLE OFF CACHE BOOL "KB" FORCE)
set(SECURE_ENABLE OFF CACHE BOOL "KB" FORCE)
set(ACHORDION_ENABLE OFF CACHE BOOL "KB" FORCE)
