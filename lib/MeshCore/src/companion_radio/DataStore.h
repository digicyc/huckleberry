#pragma once

#include <helpers/IdentityStore.h>
#include <helpers/ContactInfo.h>
#include <helpers/ChannelDetails.h>
#include "NodePrefs.h"

class DataStoreHost {
public:
  virtual bool onContactLoaded(const ContactInfo& contact) =0;
  virtual bool getContactForSave(uint32_t idx, ContactInfo& contact) =0;
  virtual bool onChannelLoaded(uint8_t channel_idx, const ChannelDetails& ch) =0;
  virtual bool getChannelForSave(uint8_t channel_idx, ChannelDetails& ch) =0;
};

class DataStore {
  FILESYSTEM* _fs;
  FILESYSTEM* _fsExtra;
  mesh::RTCClock* _clock;
  IdentityStore identity_store;

  void loadPrefsInt(const char *filename, NodePrefs& prefs);
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  void checkAdvBlobFile();
#endif

public:
  DataStore(FILESYSTEM& fs, mesh::RTCClock& clock);
  DataStore(FILESYSTEM& fs, FILESYSTEM& fsExtra, mesh::RTCClock& clock);
  void begin();
  bool formatFileSystem();
  FILESYSTEM* getPrimaryFS() const { return _fs; }
  FILESYSTEM* getSecondaryFS() const { return _fsExtra; }
  bool loadMainIdentity(mesh::LocalIdentity &identity);
  bool saveMainIdentity(const mesh::LocalIdentity &identity);
  void loadPrefs(NodePrefs& prefs);
  bool savePrefs(NodePrefs& prefs);
  void loadContacts(DataStoreHost* host);
  void saveContacts(DataStoreHost* host, bool (*filter)(const ContactInfo& c) = NULL);
  void loadChannels(DataStoreHost* host);
  void saveChannels(DataStoreHost* host);
  void migrateToSecondaryFS();
  uint8_t getBlobByKey(const uint8_t key[], int key_len, uint8_t dest_buf[]);
  bool putBlobByKey(const uint8_t key[], int key_len, const uint8_t src_buf[], uint8_t len);
  bool deleteBlobByKey(const uint8_t key[], int key_len);
  File openRead(const char* filename);
  File openRead(FILESYSTEM* fs, const char* filename);
  bool removeFile(const char* filename);
  bool removeFile(FILESYSTEM* fs, const char* filename);
  uint32_t getStorageUsedKb() const;
  uint32_t getStorageTotalKb() const;

  // HuckleBerry: a flash write stops the T-Deck's screen and input until it's done (the contacts file
  // takes about 0.7 s, an advert blob 0.3 s), and adverts arrive at any time. So saveContacts() and
  // putBlobByKey() only note what to write, and the app calls writePending() while it's idle.
  uint32_t pendingSince() const { return _pending_since; }  // millis() of the oldest unwritten change, or 0
  bool writePending();  // writes one pending file; false if nothing was pending

private:
  struct PendingBlob {
    uint8_t key[8];  // the prefix the blob's file is named by
    uint8_t len;     // 0 for a free slot
    uint8_t data[255];
  };
  static const int MAX_PENDING_BLOBS = 32;
  PendingBlob* _pending_blobs = NULL;
  DataStoreHost* _pending_contacts_host = NULL;
  bool (*_pending_contacts_filter)(const ContactInfo& c) = NULL;
  uint32_t _pending_since = 0;
  void notePending();
  PendingBlob* findPendingBlob(const uint8_t key[], int key_len);
  void writeContacts(DataStoreHost* host, bool (*filter)(const ContactInfo& c));
  bool writeBlob(const uint8_t key[], int key_len, const uint8_t src_buf[], uint8_t len);

  FILESYSTEM* _getContactsChannelsFS() const { if (_fsExtra) return _fsExtra; return _fs;};
};
