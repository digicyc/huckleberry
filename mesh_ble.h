// Bluetooth link to the MeshCore phone app: the Nordic UART service that MeshCore companion radios
// expose, protected by a static pairing PIN. This is MeshCore's helpers/esp32/SerialBLEInterface
// ported to the NimBLE stack that the ESP32 Arduino core is built with.
#pragma once

#include <MeshCore.h>  // lets arduino-cli find the MeshCore library for the helpers/ includes
#include <helpers/BaseSerialInterface.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLESecurity.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

class MeshBLE : public BaseSerialInterface, BLESecurityCallbacks, BLEServerCallbacks, BLECharacteristicCallbacks {
public:
    // Called with each frame the app sends, before the mesh handles it.
    typedef void (*FrameHook)(const uint8_t *frame, size_t len);

    MeshBLE();

    // The Bluetooth stack starts on the first enable(), so it costs no memory while switched off.
    // MyMesh::startInterface() calls enable(); `start_enabled` false makes that call do nothing.
    void begin(const char *name, uint32_t pin, FrameHook hook, bool start_enabled);
    void setName(const char *name);
    const char *name() const { return dev_name; }

    void enable() override;
    void disable() override;
    bool isEnabled() const override { return enabled; }
    bool isConnected() const override { return connected; }
    bool isWriteBusy() const override;
    size_t writeFrame(const uint8_t src[], size_t len) override;
    size_t checkRecvFrame(uint8_t dest[]) override;

protected:
    uint32_t onPassKeyRequest() override { return pin; }
    void onPassKeyNotify(uint32_t) override {}  // the default prints the PIN to Serial
    bool onConfirmPIN(uint32_t) override { return true; }
    bool onSecurityRequest() override { return true; }
    void onAuthenticationComplete(ble_gap_conn_desc *desc) override;
    void onConnect(BLEServer *server, ble_gap_conn_desc *desc) override;
    void onDisconnect(BLEServer *server, ble_gap_conn_desc *desc) override;
    void onWrite(BLECharacteristic *c, ble_gap_conn_desc *desc) override;

private:
    struct Frame {
        uint8_t len;
        uint8_t buf[MAX_FRAME_SIZE];
    };
    static const int QUEUE_SIZE = 4;

    void startStack();
    void clearBuffers();

    char dev_name[48];
    uint32_t pin;
    FrameHook hook;
    BLEServer *server;
    BLEService *service;
    BLECharacteristic *tx;
    volatile bool connected;  // paired and encrypted
    bool was_connected;
    bool enabled;
    bool skip_enable;
    uint16_t conn_handle;
    unsigned long last_write;
    unsigned long adv_restart_at;
    StaticQueue_t recv_queue_state;
    uint8_t recv_queue_storage[QUEUE_SIZE * sizeof(Frame)];
    QueueHandle_t recv_queue;
    int send_len;
    Frame send_queue[QUEUE_SIZE];
};
