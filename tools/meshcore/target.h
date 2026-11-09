// HuckleBerry's board definitions for MeshCore's companion radio code (companion_radio/MyMesh.cpp and
// helpers/CommonRadioPrefs.cpp include <target.h>, which MeshCore normally gets from a variant).
// The objects are defined in HuckleBerry's messenger.cpp.
#pragma once

#include <Mesh.h>
#include <helpers/radiolib/CustomSX1262Wrapper.h>
#include <helpers/SensorManager.h>
#include <helpers/KeyValueStore.h>

class DeckBoard : public mesh::MainBoard {
public:
    uint16_t getBattMilliVolts() override;
    const char *getManufacturerName() const override { return "LilyGo T-Deck"; }
    void reboot() override;
    uint8_t getStartupReason() const override { return BD_STARTUP_NORMAL; }
    void attachDynamicPrefs(KeyValueStore *) {}
};

extern DeckBoard board;
extern CustomSX1262Wrapper radio_driver;
extern SensorManager sensors;

mesh::LocalIdentity radio_new_identity();
