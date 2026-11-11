// MeshCore LoRa messaging: the T-Deck runs MeshCore's companion radio node, which the MeshCore phone
// app can use over Bluetooth, and keeps per-chat message history on the FFat partition for the
// Messages app. Every MeshCore channel (Public, hashtag and private groups) and every companion
// that has advertised gets a chat.
#pragma once
#include <stdint.h>

static const int MAX_CHAT_TEXT = 160;  // MeshCore MAX_TEXT_LEN
static const int MAX_NODE_NAME = 31;   // longest name that fits in a MeshCore advert
static const int MAX_CHANNEL_NAME = 31;

enum MessageStatus : uint8_t {
    MSG_RECEIVED,
    MSG_QUEUED,     // outgoing DM waiting for the radio
    MSG_SENT,       // channel message transmitted, or DM sent and awaiting ACK
    MSG_DELIVERED,  // DM acknowledged by the recipient
    MSG_FAILED,     // DM never acknowledged
};

static const int8_t HOPS_UNKNOWN = -1;

struct ChatMessage {
    uint32_t time;  // epoch seconds (sender's clock for received messages)
    uint8_t status;
    char sender[32];  // empty for our own messages
    char text[MAX_CHAT_TEXT + 1];
    int8_t hops;  // received messages: repeaters it came through (0 = heard directly), or HOPS_UNKNOWN
};

struct ChatSummary {
    const char *name;
    bool is_channel;
    uint8_t unread;
    uint32_t last_activity;  // epoch seconds, our clock
    const ChatMessage *last_message;  // NULL if none
};

// A node in MeshCore's contact list: companions, repeaters, room servers and sensors that advertised.
enum ContactType : uint8_t { CONTACT_CHAT = 1, CONTACT_REPEATER = 2, CONTACT_ROOM = 3, CONTACT_SENSOR = 4 };
struct ContactSummary {
    char name[32];
    uint8_t type;           // ContactType
    uint8_t key[6];         // public key prefix
    int8_t hops;            // hops on the known route, or -1 for none (flood)
    uint32_t last_heard;    // epoch seconds, our clock
    bool has_location;
    double lat, lon;        // from its last advert
};

// Called when a chat changes. `incoming` is true for a newly received message.
typedef void (*MessengerListener)(int chat, bool incoming);

bool messenger_init();  // false if the radio failed to start
void messenger_loop();

// Chat ids are stable while running. Fills `ids` (channels in MeshCore's order, then direct
// messages, most recent first).
int messenger_chats(int *ids, int max);
bool messenger_chat_summary(int chat, ChatSummary &out);
int messenger_message_count(int chat);
uint32_t messenger_message_total(int chat);  // messages ever added (count stops at the newest 60)
const ChatMessage *messenger_message(int chat, int index);  // 0 = oldest
bool messenger_send(int chat, const char *text);
int messenger_max_text(int chat);  // longest message that fits
void messenger_mark_read(int chat);
int messenger_unread_total();
void messenger_clear_chat(int chat);  // forgets its messages

// Group channels. A name starting with '#' is a hashtag channel, whose key comes from the name;
// otherwise `key_hex` is the 32-hex-digit (16-byte) key, or NULL for a new random one. Returns the
// chat id, or -1 (no free channel slot or a bad key).
int messenger_add_channel(const char *name, const char *key_hex);
bool messenger_remove_channel(int chat);  // not the Public channel
bool messenger_channel_key(int chat, char *hex33);  // the key to share for a channel chat
bool messenger_is_public(int chat);

// Contacts, most recently heard first, and the direct-message chat for one.
int messenger_contacts(ContactSummary *out, int max);
int messenger_contact_chat(const uint8_t *key6);  // -1 if it's not a companion we know
bool messenger_chat_contact(int chat, ContactSummary &out);  // the contact a direct-message chat is with
bool messenger_remove_contact(const uint8_t *key6);

void messenger_send_advert();  // flood advert so others can add us
const char *messenger_node_name();
void messenger_set_node_name(const char *name);
void messenger_set_listener(MessengerListener listener);
void messenger_set_location(double lat, double lon, double alt_m);  // from the GPS
bool messenger_share_location();  // include the position in adverts
void messenger_set_share_location(bool on);
bool messenger_radio_ok();

// The LoRa settings the MeshCore app has: radio parameters, the path hash size and a few mesh
// options. They're MeshCore's own prefs, so the phone app sees changes made here and vice versa.
static const int MIN_TX_DBM = -9, MAX_TX_DBM = LORA_TX_POWER;
struct RadioSettings {
    float freq;               // MHz
    float bw;                 // kHz
    uint8_t sf;               // spreading factor, 5-12
    uint8_t cr;               // coding rate 4/5 to 4/8, as 5-8
    int8_t tx_dbm;            // MIN_TX_DBM to MAX_TX_DBM
    uint8_t path_hash_bytes;  // 1-3: bytes per repeater in a message's path
    bool multi_acks;          // send delivery receipts twice
    bool auto_add;            // add every node heard to the contacts
};
bool messenger_radio_settings(RadioSettings &out);
// Applies and saves them; false (and no change) if a value is out of range.
bool messenger_set_radio_settings(const RadioSettings &s);

struct RadioStats {
    int noise_floor;           // dBm
    float last_rssi, last_snr;  // of the last packet received
    uint32_t received, sent;   // packets since boot
    uint32_t airtime_s;        // transmitting, since boot
};
void messenger_radio_stats(RadioStats &out);

// Bluetooth link for the MeshCore phone app. The on/off setting is saved.
bool messenger_bt_enabled();
void messenger_bt_set_enabled(bool on);
bool messenger_bt_connected();  // a phone is paired and connected
const char *messenger_bt_name();  // what the phone app lists, "MeshCore-<node name>"
uint32_t messenger_bt_pin();  // six-digit pairing PIN
