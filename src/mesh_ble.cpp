#include "mesh_ble.h"
#include <host/ble_gap.h>
#include <services/gap/ble_svc_gap.h>

#define SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"  // Nordic UART service
#define RX_UUID "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define TX_UUID "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

static const unsigned long ADV_RESTART_DELAY_MS = 1000;
static const unsigned long WRITE_MIN_INTERVAL_MS = 60;  // space notifications like MeshCore does

MeshBLE::MeshBLE()
    : pin(0), hook(NULL), server(NULL), service(NULL), tx(NULL), connected(false), was_connected(false),
      enabled(false), skip_enable(false), conn_handle(0), last_write(0), adv_restart_at(0), send_len(0)
{
    dev_name[0] = '\0';
    recv_queue = xQueueCreateStatic(QUEUE_SIZE, sizeof(Frame), recv_queue_storage, &recv_queue_state);
}

void MeshBLE::begin(const char *name, uint32_t pin_code, FrameHook frame_hook, bool start_enabled)
{
    skip_enable = !start_enabled;
    strlcpy(dev_name, name, sizeof(dev_name));
    pin = pin_code;
    hook = frame_hook;
}

void MeshBLE::startStack()
{
    BLEDevice::init(dev_name);
    BLEDevice::setSecurityCallbacks(this);
    BLEDevice::setMTU(MAX_FRAME_SIZE);

    // The phone asks for the PIN; the T-Deck shows it in the Bluetooth app.
    BLESecurity::setCapability(ESP_IO_CAP_OUT);
    BLESecurity::setPassKey(true, pin);
    BLESecurity::setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);

    server = BLEDevice::createServer();
    server->setCallbacks(this);
    service = server->createService(SERVICE_UUID);
    tx = service->createCharacteristic(TX_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY |
                                                    BLECharacteristic::PROPERTY_READ_ENC | BLECharacteristic::PROPERTY_READ_AUTHEN);
    BLECharacteristic *rx = service->createCharacteristic(
        RX_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_ENC | BLECharacteristic::PROPERTY_WRITE_AUTHEN);
    rx->setCallbacks(this);
    service->start();
    server->getAdvertising()->addServiceUUID(SERVICE_UUID);
}

void MeshBLE::setName(const char *name)
{
    strlcpy(dev_name, name, sizeof(dev_name));
    if (!server) {
        return;  // used when the stack starts
    }
    ble_svc_gap_device_name_set(dev_name);
    BLEAdvertising *adv = server->getAdvertising();
    adv->setName(dev_name);
    if (enabled && !connected) {
        adv->stop();
        adv->start();
    }
}

void MeshBLE::onAuthenticationComplete(ble_gap_conn_desc *desc)
{
    if (desc->sec_state.encrypted && desc->sec_state.authenticated) {
        connected = true;
    } else {
        server->disconnect(desc->conn_handle);
    }
}

void MeshBLE::onConnect(BLEServer *, ble_gap_conn_desc *desc)
{
    conn_handle = desc->conn_handle;
}

void MeshBLE::onDisconnect(BLEServer *, ble_gap_conn_desc *)
{
    connected = false;
}

void MeshBLE::onWrite(BLECharacteristic *c, ble_gap_conn_desc *)
{
    size_t len = c->getLength();
    if (len == 0 || len > MAX_FRAME_SIZE) {
        return;
    }
    Frame frame;
    frame.len = len;
    memcpy(frame.buf, c->getData(), len);
    xQueueSend(recv_queue, &frame, 0);  // dropped if the mesh loop falls behind, as in MeshCore
}

void MeshBLE::clearBuffers()
{
    xQueueReset(recv_queue);
    send_len = 0;
}

void MeshBLE::enable()
{
    if (enabled || skip_enable) {
        skip_enable = false;
        return;
    }
    if (!server) {
        startStack();
    }
    enabled = true;
    clearBuffers();
    server->getAdvertising()->start();
    adv_restart_at = 0;
}

void MeshBLE::disable()
{
    if (!enabled) {
        return;
    }
    enabled = false;
    server->getAdvertising()->stop();
    if (server->getConnectedCount()) {
        server->disconnect(conn_handle);
    }
    connected = was_connected = false;
    adv_restart_at = 0;
    clearBuffers();
}

bool MeshBLE::isWriteBusy() const
{
    return millis() - last_write < WRITE_MIN_INTERVAL_MS;
}

size_t MeshBLE::writeFrame(const uint8_t src[], size_t len)
{
    if (!connected || len == 0 || len > MAX_FRAME_SIZE || send_len >= QUEUE_SIZE) {
        return 0;
    }
    send_queue[send_len].len = len;
    memcpy(send_queue[send_len].buf, src, len);
    send_len++;
    return len;
}

size_t MeshBLE::checkRecvFrame(uint8_t dest[])
{
    if (!enabled) {
        return 0;
    }
    if (send_len && !isWriteBusy()) {
        last_write = millis();
        tx->setValue(send_queue[0].buf, send_queue[0].len);
        tx->notify();
        memmove(send_queue, send_queue + 1, --send_len * sizeof(Frame));
    }

    Frame frame;
    if (xQueueReceive(recv_queue, &frame, 0) == pdTRUE) {
        memcpy(dest, frame.buf, frame.len);
        if (hook) {
            hook(dest, frame.len);
        }
        return frame.len;
    }

    if (connected != was_connected) {
        was_connected = connected;
        if (connected) {
            server->getAdvertising()->stop();
            adv_restart_at = 0;
        } else {
            clearBuffers();
            adv_restart_at = millis() + ADV_RESTART_DELAY_MS;
        }
    }
    // Advertise again once a phone has gone, including one that connected but never paired.
    if (!connected && !adv_restart_at && server->getConnectedCount() == 0 && !server->getAdvertising()->isAdvertising()) {
        adv_restart_at = millis() + ADV_RESTART_DELAY_MS;
    }
    if (adv_restart_at && (long)(millis() - adv_restart_at) >= 0) {
        adv_restart_at = 0;
        if (server->getConnectedCount() == 0) {
            server->getAdvertising()->start();
        }
    }
    return 0;
}
