#include "messenger.h"
#include "mesh_ble.h"
#include <LilyGoLib.h>
#include <FFat.h>
#include <Preferences.h>
#include <esp_random.h>
#include <sys/time.h>
#include <lvgl.h>
#include <MeshCore.h>  // lets arduino-cli find the MeshCore library for the helpers/ includes
#include <companion_radio/MyMesh.h>

// The mesh node is MeshCore's companion radio firmware (MyMesh), so the MeshCore phone app can use
// the T-Deck as its radio over Bluetooth. The T-Deck keeps its own chat history for the Messages
// app, fed by MyMesh's listener; MyMesh keeps the identity, contacts and channels, which the phone
// app can also change.
//
// Radio settings (LORA_FREQ, LORA_BW, ...) are global build flags in the Makefile because the
// MeshCore library headers read them too; they're the defaults for a new node. The LoRa app and the
// phone app change them; MyMesh saves its prefs.

static const int MAX_CHATS = 48;
static const int MAX_MESSAGES = 60;  // kept per chat
static const int MAX_SEND_ATTEMPTS = 3;
static const int MAX_QUEUED = 8;
static const uint32_t CHANNEL_SYNC_MS = 2000;
static const char *CHAT_DIR = "/mesh/chats";

struct Chat {
    bool used;
    bool is_channel;
    uint8_t key[PUB_KEY_SIZE];  // the contact's public key, or the channel's secret
    int8_t channel_idx;         // MeshCore channel slot; -1 for contacts
    char name[32];
    uint8_t unread;
    uint32_t last_activity;
    uint32_t total;  // messages ever added; message n is msgs[n % MAX_MESSAGES]
    bool dirty;      // changed since its file was written
    ChatMessage msgs[MAX_MESSAGES];
};

struct ChatFileHeader {
    uint32_t version;
    uint8_t is_channel;
    uint8_t key[PUB_KEY_SIZE];
    char name[32];
    uint8_t unread;
    uint32_t last_activity;
    uint32_t count;
};
// Version 3 added ChatMessage.hops in what was padding, so version 2 files load too (with no hops).
static const uint32_t CHAT_FILE_VERSION = 3;
static_assert(sizeof(ChatMessage) == 200, "chat files store ChatMessage as is");

static Chat *chats;  // MAX_CHATS entries in PSRAM
static MessengerListener listener;
static Preferences prefs;
static bool radio_ok;
static uint32_t last_channel_sync;

// The T-Deck's own DMs are sent one at a time, retried here because MyMesh leaves retries to the app.
struct MessageRef {
    int chat;
    uint32_t seq;
};
static MessageRef queue[MAX_QUEUED];
static int queue_len;
static struct {
    bool active;
    MessageRef msg;
    uint32_t ack;
    uint8_t attempt;
    uint32_t deadline;  // millis
} pending;

// ---------- Board glue (declared in MeshCore's target.h) ----------

uint16_t DeckBoard::getBattMilliVolts()
{
    return instance.getBattVoltage();
}

static bool write_dirty_chat();

void DeckBoard::reboot()
{
    while (write_dirty_chat()) {
    }
    esp_restart();
}

// MeshCore time is the system clock, which HuckleBerry keeps in UTC. The T-Deck has no RTC; GPS,
// Wi-Fi, the phone app and `make settime` set it, and it survives restarts but not power-off.
class SystemRTCClock : public mesh::RTCClock {
public:
    uint32_t getCurrentTime() override { return time(NULL); }
    void setCurrentTime(uint32_t t) override  // the phone app sets the time when it connects
    {
        struct timeval tv = {.tv_sec = (time_t)t, .tv_usec = 0};
        settimeofday(&tv, NULL);
    }
};

class HardwareRNG : public mesh::RNG {
public:
    void random(uint8_t *dest, size_t sz) override { esp_fill_random(dest, sz); }
};

static SystemRTCClock rtc_clock;
static HardwareRNG rng;
static SimpleMeshTables tables;
static CustomSX1262 lora_chip(new Module(LORA_CS, LORA_IRQ, LORA_RST, LORA_BUSY, SPI));
static DataStore store(FFat, rtc_clock);
static MeshBLE ble;

DeckBoard board;
CustomSX1262Wrapper radio_driver(lora_chip, board);
SensorManager sensors;
MyMesh the_mesh(radio_driver, rng, rtc_clock, tables, store);

mesh::LocalIdentity radio_new_identity()
{
    return mesh::LocalIdentity(&rng);
}

// ---------- Chat store ----------

static uint32_t chat_count(const Chat &c)
{
    return c.total < (uint32_t)MAX_MESSAGES ? c.total : MAX_MESSAGES;
}

static ChatMessage *message_at(int chat, uint32_t seq)
{
    Chat &c = chats[chat];
    if (seq >= c.total || c.total - seq > (uint32_t)MAX_MESSAGES) {
        return NULL;  // never added, or overwritten
    }
    return &c.msgs[seq % MAX_MESSAGES];
}

static void chat_path(int chat, char *path)
{
    const uint8_t *k = chats[chat].key;
    sprintf(path, "%s/%s%02x%02x%02x%02x%02x%02x", CHAT_DIR, chats[chat].is_channel ? "c" : "", k[0], k[1], k[2], k[3],
            k[4], k[5]);
}

// Writing a chat file takes about half a second (FFat's wear levelling erases flash), which froze the
// screen when it happened on every message. So changes mark the chat dirty, and messenger_loop()
// writes dirty chats once the screen has been left alone for a few seconds, or after a minute.
static const uint32_t SAVE_IDLE_MS = 3000, SAVE_MAX_DELAY_MS = 60000;
static uint32_t dirty_since;  // millis of the oldest unwritten change; 0 if none

static void save_chat(int chat)
{
    chats[chat].dirty = true;
    if (!dirty_since) {
        dirty_since = millis() | 1;
    }
}

static void write_chat(int chat)
{
    chats[chat].dirty = false;
    char path[48];
    chat_path(chat, path);
    File f = FFat.open(path, "w", true);
    if (!f) {
        return;
    }
    Chat &c = chats[chat];
    ChatFileHeader h = {};
    h.version = CHAT_FILE_VERSION;
    h.is_channel = c.is_channel;
    memcpy(h.key, c.key, PUB_KEY_SIZE);
    memcpy(h.name, c.name, sizeof(h.name));
    h.unread = c.unread;
    h.last_activity = c.last_activity;
    h.count = chat_count(c);
    f.write((uint8_t *)&h, sizeof(h));
    for (uint32_t seq = c.total - h.count; seq < c.total; seq++) {
        f.write((uint8_t *)message_at(chat, seq), sizeof(ChatMessage));
    }
    f.close();
}

static void delete_chat(int chat)
{
    char path[48];
    chat_path(chat, path);
    FFat.remove(path);
    memset(&chats[chat], 0, sizeof(Chat));
}

static int free_slot()
{
    for (int i = 0; i < MAX_CHATS; i++) {
        if (!chats[i].used) {
            return i;
        }
    }
    return -1;
}

static void load_chats()
{
    File dir = FFat.open(CHAT_DIR);
    if (!dir || !dir.isDirectory()) {
        return;
    }
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        ChatFileHeader h;
        if (f.read((uint8_t *)&h, sizeof(h)) != sizeof(h) || (h.version != CHAT_FILE_VERSION && h.version != 2)) {
            continue;
        }
        int slot = free_slot();
        if (slot < 0) {
            break;
        }
        Chat &c = chats[slot];
        c.used = true;
        c.is_channel = h.is_channel;
        c.channel_idx = -1;
        memcpy(c.key, h.key, PUB_KEY_SIZE);
        memcpy(c.name, h.name, sizeof(c.name));
        c.unread = h.unread;
        c.last_activity = h.last_activity;
        uint32_t count = min(h.count, (uint32_t)MAX_MESSAGES);
        c.total = 0;
        for (uint32_t i = 0; i < count && f.read((uint8_t *)&c.msgs[i], sizeof(ChatMessage)) == sizeof(ChatMessage); i++) {
            if (h.version == 2) {
                c.msgs[i].hops = HOPS_UNKNOWN;
            }
            c.total++;
        }
    }
}

static int find_chat(bool is_channel, const uint8_t *key, int key_len)
{
    for (int i = 0; i < MAX_CHATS; i++) {
        if (chats[i].used && chats[i].is_channel == is_channel && memcmp(chats[i].key, key, key_len) == 0) {
            return i;
        }
    }
    return -1;
}

static void notify(int chat, bool incoming)
{
    if (listener) {
        listener(chat, incoming);
    }
}

// Finds or creates the chat for a contact, evicting the least recently active one if full.
static int contact_chat(const uint8_t *pub_key, const char *name)
{
    int chat = find_chat(false, pub_key, PUB_KEY_SIZE);
    if (chat < 0) {
        chat = free_slot();
        if (chat < 0) {
            for (int i = 0; i < MAX_CHATS; i++) {
                if (!chats[i].is_channel && (chat < 0 || chats[i].last_activity < chats[chat].last_activity)) {
                    chat = i;
                }
            }
            delete_chat(chat);
        }
        memset(&chats[chat], 0, sizeof(Chat));
        chats[chat].used = true;
        chats[chat].channel_idx = -1;
        memcpy(chats[chat].key, pub_key, PUB_KEY_SIZE);
        chats[chat].last_activity = time(NULL);
    }
    strlcpy(chats[chat].name, name, sizeof(chats[chat].name));
    return chat;
}

// Matches the channel chats to MeshCore's channel slots, which the phone app can change: adds a chat
// for each new channel and drops the chats of removed ones.
static void sync_channels()
{
    bool seen[MAX_CHATS] = {};
    for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
        ChannelDetails ch;
        if (!the_mesh.getChannel(i, ch) || !ch.name[0]) {
            continue;
        }
        int chat = find_chat(true, ch.channel.secret, PUB_KEY_SIZE);
        if (chat < 0) {
            chat = free_slot();
            if (chat < 0) {
                continue;
            }
            memset(&chats[chat], 0, sizeof(Chat));
            chats[chat].used = true;
            chats[chat].is_channel = true;
            memcpy(chats[chat].key, ch.channel.secret, PUB_KEY_SIZE);
            strlcpy(chats[chat].name, ch.name, sizeof(chats[chat].name));
            save_chat(chat);
            notify(chat, false);
        } else if (strcmp(chats[chat].name, ch.name) != 0 || chats[chat].channel_idx != i) {
            strlcpy(chats[chat].name, ch.name, sizeof(chats[chat].name));
            save_chat(chat);
            notify(chat, false);
        }
        chats[chat].channel_idx = i;
        seen[chat] = true;
    }
    for (int i = 0; i < MAX_CHATS; i++) {
        if (chats[i].used && chats[i].is_channel && !seen[i]) {
            delete_chat(i);
            notify(i, false);
        }
    }
}

static uint32_t add_message(int chat, uint32_t time, uint8_t status, const char *sender, const char *text,
                            int8_t hops = HOPS_UNKNOWN)
{
    Chat &c = chats[chat];
    uint32_t seq = c.total++;
    ChatMessage &m = c.msgs[seq % MAX_MESSAGES];
    memset(&m, 0, sizeof(m));
    m.time = time;
    m.status = status;
    strlcpy(m.sender, sender, sizeof(m.sender));
    strlcpy(m.text, text, sizeof(m.text));
    m.hops = hops;
    c.last_activity = ::time(NULL);
    return seq;
}

static bool has_message(int chat, uint32_t time, bool incoming, const char *text)
{
    Chat &c = chats[chat];
    for (uint32_t seq = c.total - chat_count(c); seq < c.total; seq++) {
        ChatMessage *m = message_at(chat, seq);
        if ((m->status == MSG_RECEIVED) == incoming && m->time == time && strcmp(m->text, text) == 0) {
            return true;
        }
    }
    return false;
}

static void add_and_notify(int chat, uint32_t time, uint8_t status, const char *sender, const char *text,
                           int8_t hops = HOPS_UNKNOWN)
{
    add_message(chat, time, status, sender, text, hops);
    bool incoming = status == MSG_RECEIVED;
    if (incoming) {
        chats[chat].unread++;
    }
    save_chat(chat);
    notify(chat, incoming);
}

// ---------- Mesh ----------

static void finish_send(uint8_t status)
{
    pending.active = false;
    ChatMessage *m = message_at(pending.msg.chat, pending.msg.seq);
    if (m) {
        m->status = status;
        save_chat(pending.msg.chat);
        notify(pending.msg.chat, false);
    }
}

static void start_send()
{
    ChatMessage *m = message_at(pending.msg.chat, pending.msg.seq);
    ContactInfo *contact = the_mesh.lookupContactByPubKey(chats[pending.msg.chat].key, PUB_KEY_SIZE);
    uint32_t est_timeout;
    if (!m || !contact ||
        the_mesh.sendMessage(*contact, m->time, pending.attempt, m->text, pending.ack, est_timeout) == MSG_SEND_FAILED) {
        finish_send(MSG_FAILED);
        return;
    }
    pending.deadline = millis() + est_timeout;
    m->status = MSG_SENT;
}

static void send_timed_out()
{
    if (++pending.attempt >= MAX_SEND_ATTEMPTS) {
        finish_send(MSG_FAILED);
        return;
    }
    if (pending.attempt == MAX_SEND_ATTEMPTS - 1) {
        // Last try: forget the learned route and flood instead.
        ContactInfo *contact = the_mesh.lookupContactByPubKey(chats[pending.msg.chat].key, PUB_KEY_SIZE);
        if (contact) {
            the_mesh.resetPathTo(*contact);
        }
    }
    start_send();
}

// A path length (packet or contact) keeps the hash size in its top two bits and the count below.
static int8_t path_hops(uint8_t path_len)
{
    return path_len == OUT_PATH_UNKNOWN ? HOPS_UNKNOWN : path_len & 63;
}

// How many repeaters a received message came through. A flooded packet collects one path entry per
// repeater. A direct one follows a route learned earlier and arrives with its path used up; it came
// back along our route to the sender, so that route's length stands in.
static int8_t message_hops(const mesh::Packet *pkt, const ContactInfo *from)
{
    if (pkt->isRouteFlood()) {
        return pkt->getPathHashCount();
    }
    return from ? path_hops(from->out_path_len) : HOPS_UNKNOWN;
}

class DeckListener : public MyMesh::Listener {
public:
    void onMessageRecv(mesh::Packet *pkt, const ContactInfo &from, uint8_t txt_type, uint32_t sender_timestamp,
                       const char *text) override
    {
        if (txt_type != TXT_TYPE_PLAIN && txt_type != TXT_TYPE_SIGNED_PLAIN) {
            return;  // CLI traffic for the phone app
        }
        Serial.printf("DM from %s: %s\n", from.name, text);
        int chat = contact_chat(from.id.pub_key, from.name);
        // Retries of the same message arrive with the same timestamp and text.
        if (!has_message(chat, sender_timestamp, true, text)) {
            add_and_notify(chat, sender_timestamp, MSG_RECEIVED, from.name, text, message_hops(pkt, &from));
        }
    }

    void onChannelMessageRecv(mesh::Packet *pkt, ChannelDetails &channel, const char *text) override
    {
        Serial.printf("%s: %s\n", channel.name, text);
        sync_channels();
        int chat = find_chat(true, channel.channel.secret, PUB_KEY_SIZE);
        if (chat < 0) {
            return;
        }
        // Channel messages are "<sender>: <text>".
        char sender[32] = "";
        const char *sep = strstr(text, ": ");
        if (sep && sep - text < (int)sizeof(sender)) {
            memcpy(sender, text, sep - text);
            sender[sep - text] = '\0';
            text = sep + 2;
        }
        add_and_notify(chat, time(NULL), MSG_RECEIVED, sender, text, message_hops(pkt, NULL));
    }

    void onQueueSizeChanged(int) override {}

    void onDiscoveredContact(ContactInfo &contact, bool is_new, uint8_t path_len, const uint8_t *path) override
    {
        Serial.printf("Advert: %s (type %d)\n", contact.name, contact.type);
        int chat = find_chat(false, contact.id.pub_key, PUB_KEY_SIZE);
        if (contact.type == ADV_TYPE_CHAT && chat >= 0 && strcmp(chats[chat].name, contact.name) != 0) {
            strlcpy(chats[chat].name, contact.name, sizeof(chats[chat].name));  // renamed
            save_chat(chat);
        }
        notify(-1, false);  // the contact list changed
    }

    void onACKRecv(uint32_t ack) override
    {
        if (pending.active && ack == pending.ack) {
            finish_send(MSG_DELIVERED);
        }
    }
};

static DeckListener deck_listener;

// Messages sent from the phone app also go into the T-Deck's chats, and channel changes show up
// right away. Frame layouts are MyMesh's CMD_SEND_TXT_MSG, CMD_SEND_CHANNEL_TXT_MSG and
// CMD_SET_CHANNEL.
static bool resync_after_frame;

static void on_app_frame(const uint8_t *f, size_t len)
{
    char text[MAX_CHAT_TEXT + 1];
    uint32_t timestamp;
    if (f[0] == 2 && len >= 14 && f[1] == TXT_TYPE_PLAIN) {
        memcpy(&timestamp, &f[3], 4);
        ContactInfo *contact = the_mesh.lookupContactByPubKey(&f[7], 6);
        if (!contact || contact->type != ADV_TYPE_CHAT) {
            return;
        }
        strlcpy(text, (const char *)&f[13], min(len - 13 + 1, sizeof(text)));
        int chat = contact_chat(contact->id.pub_key, contact->name);
        if (!has_message(chat, timestamp, false, text)) {  // the app retries with the same timestamp
            add_and_notify(chat, timestamp, MSG_SENT, "", text);
        }
    } else if (f[0] == 3 && len >= 7 && f[1] == TXT_TYPE_PLAIN) {
        ChannelDetails ch;
        if (!the_mesh.getChannel(f[2], ch) || !ch.name[0]) {
            return;
        }
        int chat = find_chat(true, ch.channel.secret, PUB_KEY_SIZE);
        if (chat < 0) {
            return;
        }
        memcpy(&timestamp, &f[3], 4);
        strlcpy(text, (const char *)&f[7], min(len - 7 + 1, sizeof(text)));
        add_and_notify(chat, timestamp, MSG_SENT, "", text);
    } else if (f[0] == 0x20) {
        resync_after_frame = true;  // CMD_SET_CHANNEL; MyMesh applies it after this hook
    }
}

// ---------- Public API ----------

bool messenger_init()
{
    chats = (Chat *)ps_calloc(MAX_CHATS, sizeof(Chat));
    FFat.mkdir("/mesh");
    FFat.mkdir(CHAT_DIR);
    load_chats();
    prefs.begin("mesh");

    if (!instance.lockSPI()) {
        return false;
    }
    bool ok = lora_chip.std_init();
    instance.unlockSPI();
    if (!ok) {
        return false;
    }

    bool new_prefs = !FFat.exists("/prefs.json") && !FFat.exists("/new_prefs");
    store.begin();
    the_mesh.begin();
    NodePrefs *node = the_mesh.getNodePrefs();
    if (new_prefs) {
        // A friendlier default than MyMesh's hex name.
        snprintf(node->node_name, sizeof(node->node_name), "TDeck-%02X%02X", the_mesh.self_id.pub_key[0],
                 the_mesh.self_id.pub_key[1]);
    }
    if (new_prefs || node->ble_pin == 0) {
        if (node->ble_pin == 0) {  // a fixed random PIN for pairing the phone, shown in the Bluetooth app
            node->ble_pin = 100000 + esp_random() % 900000;
            the_mesh.setBLEPin(node->ble_pin);
        }
        the_mesh.savePrefs();
    }

    char ble_name[48];
    snprintf(ble_name, sizeof(ble_name), BLE_NAME_PREFIX "%s", node->node_name);
    ble.begin(ble_name, the_mesh.getBLEPin(), on_app_frame, prefs.getBool("bt", true));
    the_mesh.startInterface(ble);
    the_mesh.setListener(&deck_listener);
    radio_ok = true;
    sync_channels();

    // Let nodes in direct range know we're here; a flood advert is sent on request.
    the_mesh.advert();
    Serial.printf("MeshCore node %s on %.3f MHz SF%d BW%.1f, Bluetooth %s PIN %06u\n", node->node_name,
                  (double)node->freq, node->sf, (double)node->bw, ble.isEnabled() ? "on" : "off", the_mesh.getBLEPin());
    return true;
}

// Writes one dirty chat's file; false if none was dirty.
static bool write_dirty_chat()
{
    for (int i = 0; i < MAX_CHATS; i++) {
        if (chats[i].used && chats[i].dirty) {
            write_chat(i);
            return true;
        }
    }
    dirty_since = 0;
    return false;
}

void messenger_loop()
{
    if (dirty_since &&
        (lv_display_get_inactive_time(NULL) > SAVE_IDLE_MS || millis() - dirty_since > SAVE_MAX_DELAY_MS)) {
        write_dirty_chat();  // one per loop, so a key or tap in between still gets through
    }
    if (!radio_ok) {
        return;
    }
    the_mesh.loop();
    if (resync_after_frame || millis() - last_channel_sync > CHANNEL_SYNC_MS) {
        resync_after_frame = false;
        last_channel_sync = millis();
        sync_channels();
    }
    if (pending.active && (long)(millis() - pending.deadline) >= 0) {
        send_timed_out();
    }
    if (!pending.active && queue_len) {
        pending.msg = queue[0];
        memmove(queue, queue + 1, --queue_len * sizeof(queue[0]));
        pending.attempt = 0;
        pending.active = true;
        start_send();
    }
}

int messenger_chats(int *ids, int max)
{
    int n = 0;
    for (int i = 0; i < MAX_CHATS && n < max; i++) {
        if (chats[i].used) {
            ids[n++] = i;
        }
    }
    // Channels first in slot order, then direct messages by most recent activity.
    auto before = [](const Chat &a, const Chat &b) {
        if (a.is_channel != b.is_channel) {
            return a.is_channel;
        }
        return a.is_channel ? a.channel_idx < b.channel_idx : a.last_activity > b.last_activity;
    };
    for (int i = 1; i < n; i++) {
        for (int j = i; j > 0 && before(chats[ids[j]], chats[ids[j - 1]]); j--) {
            int tmp = ids[j];
            ids[j] = ids[j - 1];
            ids[j - 1] = tmp;
        }
    }
    return n;
}

bool messenger_chat_summary(int chat, ChatSummary &out)
{
    if (chat < 0 || chat >= MAX_CHATS || !chats[chat].used) {
        return false;
    }
    Chat &c = chats[chat];
    out.name = c.name;
    out.is_channel = c.is_channel;
    out.unread = c.unread;
    out.last_activity = c.last_activity;
    out.last_message = c.total ? message_at(chat, c.total - 1) : NULL;
    return true;
}

int messenger_message_count(int chat)
{
    return chat_count(chats[chat]);
}

uint32_t messenger_message_total(int chat)
{
    return chats[chat].total;
}

const ChatMessage *messenger_message(int chat, int index)
{
    Chat &c = chats[chat];
    return message_at(chat, c.total - chat_count(c) + index);
}

int messenger_max_text(int chat)
{
    // Channel messages go out as "<node name>: <text>", and MeshCore truncates the whole thing.
    return chats[chat].is_channel ? MAX_CHAT_TEXT - strlen(messenger_node_name()) - 2 : MAX_CHAT_TEXT;
}

bool messenger_send(int chat, const char *text)
{
    if (!radio_ok || chat < 0 || chat >= MAX_CHATS || !chats[chat].used) {
        return false;
    }
    Chat &c = chats[chat];
    uint32_t now = time(NULL);
    if (c.is_channel) {
        ChannelDetails ch;
        bool ok = c.channel_idx >= 0 && the_mesh.getChannel(c.channel_idx, ch) &&
                  the_mesh.sendGroupMessage(now, ch.channel, the_mesh.getNodeName(), text, strlen(text));
        add_message(chat, now, ok ? MSG_SENT : MSG_FAILED, "", text);
    } else {
        if (queue_len == MAX_QUEUED) {
            return false;
        }
        queue[queue_len++] = {chat, add_message(chat, now, MSG_QUEUED, "", text)};
    }
    save_chat(chat);
    notify(chat, false);
    return true;
}

void messenger_mark_read(int chat)
{
    if (chats[chat].unread) {
        chats[chat].unread = 0;
        save_chat(chat);
    }
}

int messenger_unread_total()
{
    int total = 0;
    for (int i = 0; i < MAX_CHATS; i++) {
        total += chats[i].used ? chats[i].unread : 0;
    }
    return total;
}

void messenger_clear_chat(int chat)
{
    if (pending.active && pending.msg.chat == chat) {
        pending.active = false;
    }
    chats[chat].total = 0;
    chats[chat].unread = 0;
    save_chat(chat);
    notify(chat, false);
}

static bool parse_hex(const char *hex, uint8_t *out, int n)
{
    if ((int)strlen(hex) != 2 * n) {
        return false;
    }
    for (int i = 0; i < n; i++) {
        char byte[3] = {hex[2 * i], hex[2 * i + 1], 0};
        char *end;
        out[i] = strtoul(byte, &end, 16);
        if (*end) {
            return false;
        }
    }
    return true;
}

int messenger_add_channel(const char *name, const char *key_hex)
{
    if (!radio_ok || !name[0]) {
        return -1;
    }
    ChannelDetails ch = {};
    strlcpy(ch.name, name, sizeof(ch.name));
    if (name[0] == '#') {
        mesh::Utils::sha256(ch.channel.secret, 16, (const uint8_t *)name, strlen(name));
    } else if (key_hex) {
        if (!parse_hex(key_hex, ch.channel.secret, 16)) {
            return -1;
        }
    } else {
        esp_fill_random(ch.channel.secret, 16);
    }
    int existing = find_chat(true, ch.channel.secret, PUB_KEY_SIZE);
    if (existing >= 0) {
        return existing;  // already joined
    }
    for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
        ChannelDetails slot;
        if (the_mesh.getChannel(i, slot) && !slot.name[0]) {
            the_mesh.setChannel(i, ch);
            store.saveChannels(&the_mesh);
            sync_channels();
            return find_chat(true, ch.channel.secret, PUB_KEY_SIZE);
        }
    }
    return -1;
}

bool messenger_is_public(int chat)
{
    return chats[chat].is_channel && chats[chat].channel_idx == 0;
}

bool messenger_remove_channel(int chat)
{
    if (!chats[chat].is_channel || chats[chat].channel_idx <= 0) {
        return false;
    }
    ChannelDetails empty = {};
    the_mesh.setChannel(chats[chat].channel_idx, empty);
    store.saveChannels(&the_mesh);
    sync_channels();
    return true;
}

bool messenger_channel_key(int chat, char *hex33)
{
    if (!chats[chat].is_channel) {
        return false;
    }
    for (int i = 0; i < 16; i++) {
        sprintf(hex33 + 2 * i, "%02x", chats[chat].key[i]);
    }
    return true;
}

static void summarize(const ContactInfo &c, ContactSummary &s)
{
    strlcpy(s.name, c.name, sizeof(s.name));
    s.type = c.type;
    memcpy(s.key, c.id.pub_key, sizeof(s.key));
    s.hops = path_hops(c.out_path_len);
    s.last_heard = c.lastmod;
    s.has_location = c.gps_lat || c.gps_lon;
    s.lat = c.gps_lat / 1e6;
    s.lon = c.gps_lon / 1e6;
}

int messenger_contacts(ContactSummary *out, int max)
{
    int n = 0;
    ContactInfo c;
    for (int i = 0; n < max && the_mesh.getContactByIdx(i, c); i++) {
        summarize(c, out[n++]);
    }
    for (int i = 1; i < n; i++) {
        for (int j = i; j > 0 && out[j].last_heard > out[j - 1].last_heard; j--) {
            ContactSummary tmp = out[j];
            out[j] = out[j - 1];
            out[j - 1] = tmp;
        }
    }
    return n;
}

int messenger_contact_chat(const uint8_t *key6)
{
    ContactInfo *contact = the_mesh.lookupContactByPubKey(key6, 6);
    if (!contact || contact->type != ADV_TYPE_CHAT) {
        return -1;
    }
    int chat = contact_chat(contact->id.pub_key, contact->name);
    save_chat(chat);
    return chat;
}

bool messenger_chat_contact(int chat, ContactSummary &out)
{
    if (chat < 0 || chat >= MAX_CHATS || !chats[chat].used || chats[chat].is_channel) {
        return false;
    }
    ContactInfo *contact = the_mesh.lookupContactByPubKey(chats[chat].key, PUB_KEY_SIZE);
    if (!contact) {
        return false;
    }
    summarize(*contact, out);
    return true;
}

bool messenger_remove_contact(const uint8_t *key6)
{
    ContactInfo *contact = the_mesh.lookupContactByPubKey(key6, 6);
    if (!contact) {
        return false;
    }
    int chat = find_chat(false, contact->id.pub_key, PUB_KEY_SIZE);
    if (chat >= 0) {
        delete_chat(chat);
    }
    bool ok = the_mesh.removeContact(*contact);
    notify(-1, false);
    return ok;
}

void messenger_send_advert()
{
    mesh::Packet *advert = messenger_share_location()
                               ? the_mesh.createSelfAdvert(the_mesh.getNodeName(), sensors.node_lat, sensors.node_lon)
                               : the_mesh.createSelfAdvert(the_mesh.getNodeName());
    if (advert) {
        the_mesh.sendFlood(advert);
    }
}

const char *messenger_node_name()
{
    return the_mesh.getNodeName();
}

void messenger_set_node_name(const char *name)
{
    strlcpy(the_mesh.getNodePrefs()->node_name, name, sizeof(the_mesh.getNodePrefs()->node_name));
    the_mesh.savePrefs();
    char ble_name[48];
    snprintf(ble_name, sizeof(ble_name), BLE_NAME_PREFIX "%s", name);
    ble.setName(ble_name);
}

void messenger_set_listener(MessengerListener l)
{
    listener = l;
}

void messenger_set_location(double lat, double lon, double alt_m)
{
    sensors.node_lat = lat;
    sensors.node_lon = lon;
    sensors.node_altitude = alt_m;
}

bool messenger_share_location()
{
    return radio_ok && the_mesh.getNodePrefs()->advert_loc_policy != ADVERT_LOC_NONE;
}

void messenger_set_share_location(bool on)
{
    if (!radio_ok) {
        return;
    }
    the_mesh.getNodePrefs()->advert_loc_policy = on ? ADVERT_LOC_SHARE : ADVERT_LOC_NONE;
    the_mesh.savePrefs();
}

bool messenger_radio_ok()
{
    return radio_ok;
}

bool messenger_radio_settings(RadioSettings &out)
{
    if (!radio_ok) {
        return false;
    }
    const NodePrefs *p = the_mesh.getNodePrefs();
    out.freq = p->freq;
    out.bw = p->bw;
    out.sf = p->sf;
    out.cr = p->cr;
    out.tx_dbm = p->tx_power_dbm;
    out.path_hash_bytes = p->path_hash_mode + 1;
    out.multi_acks = p->multi_acks != 0;
    out.auto_add = (p->manual_add_contacts & 1) == 0;
    return true;
}

// The same checks and steps as MyMesh's CMD_SET_RADIO_PARAMS, CMD_SET_RADIO_TX_POWER,
// CMD_SET_PATH_HASH_MODE and CMD_SET_OTHER_PARAMS, which the phone app uses. The frequency range is
// the SX1262's.
bool messenger_set_radio_settings(const RadioSettings &s)
{
    if (!radio_ok || s.freq < 150 || s.freq > 960 || s.bw < 7 || s.bw > 500 || s.sf < 5 || s.sf > 12 || s.cr < 5 ||
        s.cr > 8 || s.tx_dbm < MIN_TX_DBM || s.tx_dbm > MAX_TX_DBM || s.path_hash_bytes < 1 || s.path_hash_bytes > 3) {
        return false;
    }
    NodePrefs *p = the_mesh.getNodePrefs();
    bool radio_changed = s.freq != p->freq || s.bw != p->bw || s.sf != p->sf || s.cr != p->cr;
    bool power_changed = s.tx_dbm != p->tx_power_dbm;
    p->freq = s.freq;
    p->bw = s.bw;
    p->sf = s.sf;
    p->cr = s.cr;
    p->tx_power_dbm = s.tx_dbm;
    p->path_hash_mode = s.path_hash_bytes - 1;
    p->multi_acks = s.multi_acks;
    p->manual_add_contacts = (p->manual_add_contacts & ~1) | (s.auto_add ? 0 : 1);
    the_mesh.savePrefs();
    if (radio_changed) {
        radio_driver.setParams(p->freq, p->bw, p->sf, p->cr);
        Serial.printf("Radio set to %.3f MHz BW%.1f SF%d CR%d\n", (double)p->freq, (double)p->bw, p->sf, p->cr);
    }
    if (power_changed) {
        radio_driver.setTxPower(p->tx_power_dbm);
    }
    return true;
}

void messenger_radio_stats(RadioStats &out)
{
    out.noise_floor = radio_driver.getNoiseFloor();
    out.last_rssi = radio_driver.getLastRSSI();
    out.last_snr = radio_driver.getLastSNR();
    out.received = radio_driver.getPacketsRecv();
    out.sent = radio_driver.getPacketsSent();
    out.airtime_s = the_mesh.getTotalAirTime() / 1000;
}

bool messenger_bt_enabled()
{
    return ble.isEnabled();
}

void messenger_bt_set_enabled(bool on)
{
    if (!radio_ok) {
        return;
    }
    on ? ble.enable() : ble.disable();
    prefs.putBool("bt", on);
}

bool messenger_bt_connected()
{
    return ble.isConnected();
}

const char *messenger_bt_name()
{
    return ble.name();
}

uint32_t messenger_bt_pin()
{
    return the_mesh.getBLEPin();
}
