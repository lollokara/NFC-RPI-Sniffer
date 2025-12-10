#include "nfc.h"
#include <Arduino.h>
#include <Adafruit_PN532.h>
#include <ArduinoJson.h>
#include "config.h"
#include "website.h"
#include "api.h"
#include "esp_task_wdt.h"
#include "scale.h"
#include "bambu.h"
#include "main.h"
#include "debug.h"

//Adafruit_PN532 nfc(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS);
Adafruit_PN532 nfc(PN532_IRQ, PN532_RESET);

TaskHandle_t RfidReaderTask;

JsonDocument rfidData;
String activeSpoolId = "";
String lastSpoolId = "";
String nfcJsonData = "";
bool tagProcessed = false;
volatile bool pauseBambuMqttTask = false;
volatile bool nfcReadingTaskSuspendRequest = false;
volatile bool nfcReadingTaskSuspendState = false;
volatile bool nfcWriteInProgress = false; // Prevent any tag operations during write

struct NfcWriteParameterType {
  bool tagType;
  char* payload;
};

volatile nfcReaderStateType nfcReaderState = NFC_IDLE;

// Helper to manually recover I2C bus
void recoverI2C() {
    Serial.println("[NFC] Attempting I2C Bus Recovery...");
    // Manually toggle SCL to release stuck slaves
    // Pins 8 (SDA) and 9 (SCL) for ESP32-C3 SuperMini default I2C
    int sda = 8;
    int scl = 9;

    pinMode(sda, INPUT_PULLUP); // Make sure SDA is released
    pinMode(scl, OUTPUT);

    // Toggle SCL clock 9 times
    for (int i = 0; i < 9; i++) {
        digitalWrite(scl, HIGH);
        delayMicroseconds(10);
        digitalWrite(scl, LOW);
        delayMicroseconds(10);
    }
    digitalWrite(scl, HIGH); // Release SCL
    pinMode(scl, INPUT_PULLUP);

    delay(10);
    Serial.println("[NFC] I2C Bus Recovery Sequence Complete");
}

// Helper to read tag size
uint16_t readTagSize()
{
  uint8_t buffer[4];
  memset(buffer, 0, 4);
  nfc.ntag2xx_ReadPage(3, buffer);
  return buffer[2]*8;
}

// Robust page reading with error recovery
bool robustPageRead(uint8_t page, uint8_t* buffer) {
    const int MAX_READ_ATTEMPTS = 3;
    
    for (int attempt = 0; attempt < MAX_READ_ATTEMPTS; attempt++) {
        esp_task_wdt_reset();
        // yield();
        vTaskDelay(pdMS_TO_TICKS(1)); // Context switch
        
        PROFILE_SCOPE("nfc.ntag2xx_ReadPage");
        // unsigned long start = millis();
        bool success = nfc.ntag2xx_ReadPage(page, buffer);

        if (success) {
            return true;
        }
        
        Serial.printf("Page %d read failed, attempt %d/%d\n", page, attempt + 1, MAX_READ_ATTEMPTS);
        
        // Try to stabilize connection between attempts
        if (attempt < MAX_READ_ATTEMPTS - 1) {
            vTaskDelay(pdMS_TO_TICKS(25));
            
            // Re-verify tag presence with quick check
            uint8_t uid[7];
            uint8_t uidLength;
            // unsigned long startCheck = millis();
            if (!nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 100)) {
                // Serial.printf("[PERF_DEBUG] Tag lost check took %lu ms\n", millis() - startCheck);
                Serial.println("Tag lost during read operation");
                return false;
            }
        }
    }
    
    return false;
}

// Function to decode NDEF and return JSON - MOVED UP before it's used
bool decodeNdefAndReturnJson(const byte* encodedMessage, String uidString) {
  PROFILE_FUNCTION();
  oledShowProgressBar(1, octoEnabled?5:4, "Reading", "Decoding data");

  // Debug: Print first 32 bytes of the raw data
  // Serial.println("Raw NDEF data (first 32 bytes):");
  // for (int i = 0; i < 32; i++) {
  //   if (encodedMessage[i] < 0x10) Serial.print("0");
  //   Serial.print(encodedMessage[i], HEX);
  //   Serial.print(" ");
  //   if ((i + 1) % 16 == 0) Serial.println();
  // }
  // Serial.println();

  // Look for the NDEF TLV structure starting from the beginning
  int tlvOffset = 0;
  bool foundNdefTlv = false;

  // Search for NDEF TLV (0x03) in the first few bytes
  for (int i = 0; i < 16; i++) {
    if (encodedMessage[i] == 0x03) {
      tlvOffset = i;
      foundNdefTlv = true;
      Serial.print("Found NDEF TLV at offset: ");
      Serial.println(tlvOffset);
      break;
    }
  }

  if (!foundNdefTlv) {
    Serial.println("No NDEF TLV found in tag data");
    return false;
  }

  // Get the NDEF message length from TLV
  uint16_t ndefMessageLength = 0;
  int ndefRecordOffset = 0;

  if (encodedMessage[tlvOffset + 1] == 0xFF) {
    // Extended length format: next 2 bytes contain the actual length
    ndefMessageLength = (encodedMessage[tlvOffset + 2] << 8) | encodedMessage[tlvOffset + 3];
    ndefRecordOffset = tlvOffset + 4; // Skip TLV tag + 0xFF + 2 length bytes
    Serial.print("NDEF Message Length (extended): ");
  } else {
    // Standard length format: single byte contains the length
    ndefMessageLength = encodedMessage[tlvOffset + 1];
    ndefRecordOffset = tlvOffset + 2; // Skip TLV tag + 1 length byte
    Serial.print("NDEF Message Length (standard): ");
  }
  Serial.println(ndefMessageLength);

  // Get pointer to NDEF record
  const byte* ndefRecord = &encodedMessage[ndefRecordOffset];

  // Parse NDEF record header
  byte recordHeader = ndefRecord[0];
  byte typeLength = ndefRecord[1];

  // Determine payload length (can be 1 or 4 bytes depending on SR flag)
  uint32_t payloadLength = 0;
  byte payloadLengthBytes = 1;
  byte payloadLengthOffset = 2;

  // Check if Short Record (SR) flag is set (bit 4)
  if (recordHeader & 0x10) { // SR flag
    payloadLength = ndefRecord[2];
    payloadLengthBytes = 1;
    payloadLengthOffset = 2;
  } else {
    // Long record format (4 bytes for payload length)
    payloadLength = (ndefRecord[2] << 24) | (ndefRecord[3] << 16) |
                   (ndefRecord[4] << 8) | ndefRecord[5];
    payloadLengthBytes = 4;
    payloadLengthOffset = 2;
  }

  // Check for ID field (if IL flag is set)
  byte idLength = 0;
  if (recordHeader & 0x08) { // IL flag
    idLength = ndefRecord[payloadLengthOffset + payloadLengthBytes];
  }

  // Calculate offset to payload
  byte payloadOffset = 1 + 1 + payloadLengthBytes + typeLength + idLength;

  // Verify we have enough data
  if (payloadOffset + payloadLength > ndefMessageLength) {
    Serial.println("Invalid NDEF structure - payload extends beyond message");
    return false;
  }

  nfcJsonData = "";

  // Extract JSON payload with validation
  uint32_t actualJsonLength = 0;
  for (uint32_t i = 0; i < payloadLength; i++) {
    byte currentByte = ndefRecord[payloadOffset + i];

    // Stop at null terminator or if we find the end of JSON
    if (currentByte == 0x00) {
      break;
    }

    // Only add printable characters and common JSON characters
    if (currentByte >= 32 && currentByte <= 126) {
      nfcJsonData += (char)currentByte;
      actualJsonLength++;
    }

    // Check if we've reached the end of a JSON object
    if (currentByte == '}') {
      // Count opening and closing braces to detect complete JSON
      int braceCount = 0;
      for (uint32_t j = 0; j <= i; j++) {
        if (ndefRecord[payloadOffset + j] == '{') braceCount++;
        else if (ndefRecord[payloadOffset + j] == '}') braceCount--;
      }

      if (braceCount == 0) {
        actualJsonLength = i + 1;
        break;
      }
    }
  }

  // Trim any trailing whitespace or invalid characters
  nfcJsonData.trim();

  // JSON-Dokument verarbeiten
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, nfcJsonData);
  if (error)
  {
    nfcJsonData = "";
    Serial.println("Fehler beim Verarbeiten des JSON-Dokuments");
    Serial.print("deserializeJson() failed: ");
    Serial.println(error.f_str());
    return false;
  }
  else
  {
    // If spoolman is unavailable, there is no point in continuing
    if(spoolmanConnected){
      // Sende die aktualisierten AMS-Daten an alle WebSocket-Clients
      Serial.println("JSON-Dokument erfolgreich verarbeitet");
      // Serial.println(doc.as<String>());
      if (doc["sm_id"].is<String>() && doc["sm_id"] != "" && doc["sm_id"] != "0")
      {
        oledShowProgressBar(2, octoEnabled?5:4, "Spool Tag", "Weighing");
        Serial.println("SPOOL-ID gefunden: " + doc["sm_id"].as<String>());
        activeSpoolId = doc["sm_id"].as<String>();
        lastSpoolId = activeSpoolId;
      }
      else if(doc["location"].is<String>() && doc["location"] != "")
      {
        Serial.println("Location Tag found!");
        String location = doc["location"].as<String>();
        if(lastSpoolId != ""){
          updateSpoolLocation(lastSpoolId, location);
        }
        else
        {
          Serial.println("Location update tag scanned without scanning spool before!");
          oledShowProgressBar(1, 1, "Failure", "Scan spool first");
        }
      }
      // Brand Filament not registered to Spoolman
      else if ((!doc["sm_id"].is<String>() || (doc["sm_id"].is<String>() && (doc["sm_id"] == "0" || doc["sm_id"] == "")))
              && doc["b"].is<String>() && doc["an"].is<String>())
      {
        doc["sm_id"] = "0"; // Ensure sm_id is set to 0
        // If no sm_id is present but the brand is Brand Filament then
        // create a new spool, maybe brand too, in Spoolman
        Serial.println("New Brand Filament Tag found!");
        createBrandFilament(doc, uidString);
      }
      else
      {
        Serial.println("Keine SPOOL-ID gefunden.");
        activeSpoolId = "";
        oledShowProgressBar(1, 1, "Failure", "Unkown tag");
      }
    }else{
      oledShowProgressBar(octoEnabled?5:4, octoEnabled?5:4, "Failure!", "Spoolman unavailable");
    }
  }

  doc.clear();

  return true;
}

// Read complete JSON data for fast-path to enable web interface display
bool readCompleteJsonForFastPath() {
    Serial.println("=== FAST-PATH: Reading complete JSON for web interface ===");

    // Read tag size first
    uint16_t tagSize = readTagSize();
    if (tagSize == 0) {
        Serial.println("FAST-PATH: Could not determine tag size");
        return false;
    }

    // Create buffer for complete data
    uint8_t* data = (uint8_t*)malloc(tagSize);
    if (!data) {
        Serial.println("FAST-PATH: Could not allocate memory for complete read");
        return false;
    }
    memset(data, 0, tagSize);

    // Read all pages
    uint8_t numPages = tagSize / 4;
    for (uint8_t i = 4; i < 4 + numPages; i++) {
        if (!robustPageRead(i, data + (i - 4) * 4)) {
            Serial.printf("FAST-PATH: Failed to read page %d\n", i);
            free(data);
            return false;
        }

        // Check for NDEF message end
        if (data[(i - 4) * 4] == 0xFE) {
            Serial.println("FAST-PATH: Found NDEF message end marker");
            break;
        }

        yield();
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    // Decode NDEF and extract JSON
    bool success = decodeNdefAndReturnJson(data, ""); // Empty UID string for fast-path

    free(data);

    if (success) {
        Serial.println("✓ FAST-PATH: Complete JSON data successfully loaded");
        Serial.print("nfcJsonData length: ");
        Serial.println(nfcJsonData.length());
    } else {
        Serial.println("✗ FAST-PATH: Failed to decode complete JSON data");
    }

    return success;
}

// Optimized JSON helper - MOVED UP
String optimizeJsonForFastPath(const char* payload) {
    JsonDocument inputDoc;
    DeserializationError error = deserializeJson(inputDoc, payload);

    if (error) {
        Serial.print("JSON optimization failed: ");
        Serial.println(error.c_str());
        return String(payload); // Return original if parsing fails
    }

    // Create optimized JSON with sm_id first
    JsonDocument optimizedDoc;

    // Always add sm_id first (even if it's "0" for brand filaments)
    if (inputDoc["sm_id"].is<String>()) {
        optimizedDoc["sm_id"] = inputDoc["sm_id"].as<String>();
    } else {
        optimizedDoc["sm_id"] = "0"; // Default for brand filaments
    }

    // Add all other keys in original order
    for (JsonPair kv : inputDoc.as<JsonObject>()) {
        String key = kv.key().c_str();
        if (key != "sm_id") { // Skip sm_id as it's already added first
            optimizedDoc[key] = kv.value();
        }
    }

    String optimizedJson;
    serializeJson(optimizedDoc, optimizedJson);

    return optimizedJson;
}

// Quick Spool Check - MOVED UP
bool quickSpoolIdCheck(String uidString) {
    // Fast-path: Read NDEF structure to quickly locate and check JSON payload
    // This dramatically speeds up known spool recognition

    // CRITICAL: Do not execute during write operations!
    if (nfcWriteInProgress) {
        Serial.println("FAST-PATH: Skipped during write operation");
        return false;
    }

    Serial.println("=== FAST-PATH: Quick sm_id Check ===");
    unsigned long start = millis();

    // Read enough pages to cover NDEF header + beginning of payload (pages 4-8 = 20 bytes)
    uint8_t ndefData[20];
    memset(ndefData, 0, 20);

    for (uint8_t page = 4; page < 9; page++) {
        if (!robustPageRead(page, ndefData + (page - 4) * 4)) {
            Serial.print("FAST-PATH: Failed to read page ");
            Serial.print(page);
            Serial.println(" - falling back to full read");
            return false; // Fall back to full read if any page read fails
        }
    }

    unsigned long duration = millis() - start;
    if(duration > 50) Serial.printf("[PERF_DEBUG] Quick sm_id Check reading took %lu ms\n", duration);

    // Look for NDEF TLV (0x03) at the beginning
    int tlvOffset = -1;
    for (int i = 0; i < 8; i++) {
        if (ndefData[i] == 0x03) {
            tlvOffset = i;
            Serial.print("Found NDEF TLV at offset: ");
            Serial.println(tlvOffset);
            break;
        }
    }

    if (tlvOffset == -1) {
        Serial.println("✗ FAST-PATH: No NDEF TLV found");
        return false;
    }

    // Parse NDEF record to find JSON payload
    int ndefRecordStart;
    if (ndefData[tlvOffset + 1] == 0xFF) {
        // Extended length format
        ndefRecordStart = tlvOffset + 4;
    } else {
        // Standard length format
        ndefRecordStart = tlvOffset + 2;
    }

    if (ndefRecordStart >= 20) {
        Serial.println("✗ FAST-PATH: NDEF record starts beyond read data");
        return false;
    }

    // Parse NDEF record header
    uint8_t recordHeader = ndefData[ndefRecordStart];
    uint8_t typeLength = ndefData[ndefRecordStart + 1];

    // Calculate payload offset
    uint8_t payloadLengthBytes = (recordHeader & 0x10) ? 1 : 4; // SR flag check
    uint8_t idLength = (recordHeader & 0x08) ? ndefData[ndefRecordStart + 2 + payloadLengthBytes + typeLength] : 0; // IL flag check

    int payloadOffset = ndefRecordStart + 1 + 1 + payloadLengthBytes + typeLength + idLength;

    // Check if payload starts within our read data
    if (payloadOffset >= 20) {
        Serial.println("✗ FAST-PATH: JSON payload starts beyond quick read data - need more pages");
        return false;
    }

    // Extract JSON payload from the available data
    String quickJson = "";
    for (int i = payloadOffset; i < 20 && i < payloadOffset + 15; i++) {
        uint8_t currentByte = ndefData[i];
        if (currentByte >= 32 && currentByte <= 126) {
            quickJson += (char)currentByte;
        }
    }

    Serial.print("Quick JSON data: ");
    Serial.println(quickJson);

    // Look for sm_id pattern in the beginning of JSON - check for known vs new spools
    if (quickJson.indexOf("\"sm_id\":\"") >= 0) {
        Serial.println("✓ FAST-PATH: sm_id field found");

        // Extract sm_id from quick data
        int smIdStart = quickJson.indexOf("\"sm_id\":\"") + 9;
        int smIdEnd = quickJson.indexOf("\"", smIdStart);

        if (smIdEnd > smIdStart && smIdEnd < quickJson.length()) {
            String quickSpoolId = quickJson.substring(smIdStart, smIdEnd);
            Serial.print("✓ Quick extracted sm_id: ");
            Serial.println(quickSpoolId);

            // Only process known spools (sm_id != "0") via fast path
            if (quickSpoolId != "0" && quickSpoolId.length() > 0) {
                Serial.println("✓ FAST-PATH: Known spool detected!");

                // Set as active spool immediately
                activeSpoolId = quickSpoolId;
                lastSpoolId = activeSpoolId;

                // Read complete JSON data for web interface display
                Serial.println("FAST-PATH: Reading complete JSON data for web interface...");
                if (readCompleteJsonForFastPath()) {
                    Serial.println("✓ FAST-PATH: Complete JSON data loaded for web interface");
                } else {
                    Serial.println("⚠ FAST-PATH: Could not read complete JSON, web interface may show limited data");
                }

                oledShowProgressBar(2, octoEnabled?5:4, "Known Spool", "Quick mode");
                Serial.println("✓ FAST-PATH SUCCESS: Known spool processed quickly");
                return true;
            } else {
                Serial.println("✗ FAST-PATH: sm_id is 0 - new brand filament, need full read");
                return false; // sm_id="0" means new brand filament, needs full processing
            }
        } else {
            Serial.println("✗ FAST-PATH: Could not extract complete sm_id value");
            return false; // Need full read to get complete sm_id
        }
    }

    Serial.println("✗ FAST-PATH: No recognizable pattern - falling back to full read");
    return false; // Fall back to full tag reading
}

String detectNtagType()
{
  // Read capability container from page 3 to determine exact NTAG type
  uint8_t ccBuffer[4];
  memset(ccBuffer, 0, 4);

  if (!nfc.ntag2xx_ReadPage(3, ccBuffer)) {
    Serial.println("Failed to read capability container");
    return "UNKNOWN";
  }

  // Also read configuration pages to get more info
  uint8_t configBuffer[4];
  memset(configBuffer, 0, 4);

  // NTAG type detection based on capability container
  // CC[2] contains the data area size in bytes / 8
  uint16_t dataAreaSize = ccBuffer[2] * 8;

  // Try to read different configuration pages to determine exact type
  String tagType = "UNKNOWN";

  // Try to read page 41 (NTAG213 ends at page 39, so this should fail)
  uint8_t testBuffer[4];
  bool canReadPage41 = nfc.ntag2xx_ReadPage(41, testBuffer);

  // Try to read page 130 (NTAG215 ends at page 129, so this should fail for NTAG213/215)
  bool canReadPage130 = nfc.ntag2xx_ReadPage(130, testBuffer);

  if (dataAreaSize <= 180 && !canReadPage41) {
    tagType = "NTAG213";
  } else if (dataAreaSize <= 540 && canReadPage41 && !canReadPage130) {
    tagType = "NTAG215";
  } else if (dataAreaSize <= 928 && canReadPage130) {
    tagType = "NTAG216";
  } else {
    // Fallback: use data area size from capability container
    if (dataAreaSize <= 180) {
      tagType = "NTAG213";
    } else if (dataAreaSize <= 540) {
      tagType = "NTAG215";
    } else {
      tagType = "NTAG216";
    }
  }

  return tagType;
}

uint16_t getAvailableUserDataSize()
{
  String tagType = detectNtagType();
  uint16_t userDataSize = 0;

  if (tagType == "NTAG213") {
    // NTAG213: User data from page 4-39 (36 pages * 4 bytes = 144 bytes)
    userDataSize = 144;
  } else if (tagType == "NTAG215") {
    // NTAG215: User data from page 4-129 (126 pages * 4 bytes = 504 bytes)
    userDataSize = 504;
  } else if (tagType == "NTAG216") {
    // NTAG216: User data from page 4-225 (222 pages * 4 bytes = 888 bytes)
    userDataSize = 888;
  } else {
    // Unknown tag type, use conservative estimate
    uint16_t tagSize = readTagSize();
    userDataSize = tagSize - 60; // Reserve 60 bytes for headers/config
  }

  return userDataSize;
}

uint16_t getMaxUserDataPages()
{
  String tagType = detectNtagType();
  uint16_t maxPages = 0;

  if (tagType == "NTAG213") {
    maxPages = 39; // Pages 4-39 are user data
  } else if (tagType == "NTAG215") {
    maxPages = 129; // Pages 4-129 are user data
  } else if (tagType == "NTAG216") {
    maxPages = 225; // Pages 4-225 are user data
  } else {
    // Conservative fallback
    maxPages = 39;
  }

  return maxPages;
}

uint8_t ntag2xx_WriteNDEF(const char *payload) {
  // Determine exact tag type and capabilities first
  String tagType = detectNtagType();
  uint16_t tagSize = readTagSize();
  uint16_t availableUserData = getAvailableUserDataSize();
  uint16_t maxWritablePage = getMaxUserDataPages();

  Serial.println("=== NFC TAG ANALYSIS ===");
  Serial.print("Tag Type: ");Serial.println(tagType);
  Serial.print("Total Tag Size: ");Serial.println(tagSize);
  Serial.print("Available User Data: ");Serial.println(availableUserData);
  Serial.print("Max Writable Page: ");Serial.println(maxWritablePage);
  Serial.println("========================");

  uint8_t pageBuffer[4] = {0, 0, 0, 0};
  Serial.println("Beginne mit dem Schreiben der NDEF-Nachricht...");

  // Figure out how long the string is
  uint16_t payloadLen = strlen(payload);
  Serial.print("Länge der Payload: ");
  Serial.println(payloadLen);

  Serial.print("Payload: ");Serial.println(payload);

  // MIME type for JSON
  const char mimeType[] = "application/json";
  uint8_t mimeTypeLen = strlen(mimeType);

  // Calculate NDEF record size
  uint8_t ndefRecordHeaderSize = 3; // Header byte + Type Length + Payload Length (short record)
  uint16_t ndefRecordSize = ndefRecordHeaderSize + mimeTypeLen + payloadLen;

  // Calculate TLV size - need to check if we need extended length format
  uint8_t tlvHeaderSize;
  uint16_t totalTlvSize;

  if (ndefRecordSize <= 254) {
    // Standard TLV format: Tag (1) + Length (1) + Value (ndefRecordSize)
    tlvHeaderSize = 2;
    totalTlvSize = tlvHeaderSize + ndefRecordSize + 1; // +1 for terminator TLV
  } else {
    // Extended TLV format: Tag (1) + 0xFF + Length (2) + Value (ndefRecordSize)
    tlvHeaderSize = 4;
    totalTlvSize = tlvHeaderSize + ndefRecordSize + 1; // +1 for terminator TLV
  }

  // Allocate memory for the complete TLV structure
  uint8_t* tlvData = (uint8_t*) malloc(totalTlvSize);
  if (tlvData == NULL) {
    Serial.println("Fehler: Nicht genug Speicher für TLV-Daten vorhanden.");
    oledShowMessage("Memory error");
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    return 0;
  }

  // Build TLV structure
  uint16_t offset = 0;

  // TLV Header
  tlvData[offset++] = 0x03; // NDEF Message TLV Tag

  if (ndefRecordSize <= 254) {
    // Standard length format
    tlvData[offset++] = (uint8_t)ndefRecordSize;
  } else {
    // Extended length format
    tlvData[offset++] = 0xFF;
    tlvData[offset++] = (uint8_t)(ndefRecordSize >> 8);  // High byte
    tlvData[offset++] = (uint8_t)(ndefRecordSize & 0xFF); // Low byte
  }

  // NDEF Record Header
  tlvData[offset++] = 0xD2; // NDEF Record Header (TNF=0x2:MIME Media + SR + ME + MB)
  tlvData[offset++] = mimeTypeLen; // Type Length
  tlvData[offset++] = (uint8_t)payloadLen; // Payload Length (short record format)

  // MIME Type
  memcpy(&tlvData[offset], mimeType, mimeTypeLen);
  offset += mimeTypeLen;

  // JSON Payload
  memcpy(&tlvData[offset], payload, payloadLen);
  offset += payloadLen;

  // Terminator TLV
  tlvData[offset] = 0xFE;

  Serial.print("Gesamt-TLV-Länge: ");
  Serial.println(offset + 1);

  // Write data to tag pages (starting from page 4)
  uint16_t bytesWritten = 0;
  uint8_t pageNumber = 4;
  uint16_t totalBytes = offset + 1;

  Serial.println();
  Serial.println("=== SCHRITT 6: SCHREIBE NEUE NDEF-DATEN ===");
  Serial.print("Schreibe ");
  Serial.print(totalBytes);
  Serial.print(" Bytes in ");
  Serial.print((totalBytes + 3) / 4); // Round up division
  Serial.println(" Seiten...");

  while (bytesWritten < totalBytes && pageNumber <= maxWritablePage) {
    // Additional safety check before writing each page
    if (pageNumber > maxWritablePage) {
      Serial.print("STOP: Reached maximum writable page ");
      Serial.println(maxWritablePage);
      break;
    }
    
    // Clear page buffer
    memset(pageBuffer, 0, 4);

    // Calculate how many bytes to write to this page
    uint16_t bytesToWrite = min(4, (int)(totalBytes - bytesWritten));

    // Copy data to page buffer
    memcpy(pageBuffer, &tlvData[bytesWritten], bytesToWrite);

    // Write page to tag with retry mechanism
    bool writeSuccess = false;
    for (int writeAttempt = 0; writeAttempt < 3; writeAttempt++) {
      if (nfc.ntag2xx_WritePage(pageNumber, pageBuffer)) {
        writeSuccess = true;
        break;
      } else {
        Serial.print("Schreibversuch ");
        Serial.print(writeAttempt + 1);
        Serial.print("/3 für Seite ");
        Serial.print(pageNumber);
        Serial.println(" fehlgeschlagen");

        if (writeAttempt < 2) {
          vTaskDelay(50 / portTICK_PERIOD_MS); // Wait before retry
        }
      }
    }

    if (!writeSuccess) {
      Serial.print("FEHLER beim Schreiben der Seite ");
      Serial.println(pageNumber);
      free(tlvData);
      return 0;
    }

    bytesWritten += bytesToWrite;
    pageNumber++;
    
    yield();
    vTaskDelay(10 / portTICK_PERIOD_MS); // Slightly increased delay between page writes
  }

  free(tlvData);

  if (bytesWritten < totalBytes) {
    Serial.println("WARNUNG: Nicht alle Daten konnten geschrieben werden!");
    return 0;
  }

  Serial.println();
  Serial.println("✓ NDEF-Nachricht erfolgreich geschrieben!");

  // CRITICAL: Allow NFC interface to stabilize after write operation
  Serial.println();
  Serial.println("=== SCHRITT 7: NFC-INTERFACE STABILISIERUNG NACH SCHREIBVORGANG ===");
  Serial.println("Stabilisiere NFC-Interface nach Schreibvorgang...");

  // Give the tag and interface time to settle after write operation
  vTaskDelay(300 / portTICK_PERIOD_MS); // Increased stabilization time

  Serial.println("==================================================================");

  return 1;
}

// Write JSON Task - MOVED UP
void writeJsonToTag(void *parameter) {
  NfcWriteParameterType* params = (NfcWriteParameterType*)parameter;

  // Gib die erstellte NDEF-Message aus
  Serial.println("Erstelle NDEF-Message...");
  Serial.println(params->payload);

  nfcReaderState = NFC_WRITING;
  nfcWriteInProgress = true; // Block high-level tag operations during write

  // Do NOT suspend the reading task - we need NFC interface for verification
  // Just use nfcWriteInProgress to prevent scanning and fast-path operations
  Serial.println("NFC Write Task starting - High-level operations blocked, low-level NFC available");

  //pauseBambuMqttTask = true;
  // aktualisieren der Website wenn sich der Status ändert
  sendNfcData();
  vTaskDelay(100 / portTICK_PERIOD_MS);
  
  // Show waiting message for tag detection
  oledShowProgressBar(0, 1, "Write Tag", "Warte auf Tag");
  
  // Wait 10sec for tag
  uint8_t success = 0;
  String uidString = "";
  for (uint16_t i = 0; i < 20; i++) {
    uint8_t uid[] = { 0, 0, 0, 0, 0, 0, 0 };  // Buffer to store the returned UID
    uint8_t uidLength;
    // yield before potentially waiting for 400ms
    yield();
    esp_task_wdt_reset();
    success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 400);
    if (success) {
      for (uint8_t i = 0; i < uidLength; i++) {
        //TBD: Rework to remove all the string operations
        uidString += String(uid[i], HEX);
        if (i < uidLength - 1) {
            uidString += ":"; // Optional: Trennzeichen hinzufügen
        }
      }
      foundNfcTag(nullptr, success);
      break;
    }

    yield();
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(1));
  }

  if (success)
  {
    oledShowProgressBar(1, 3, "Write Tag", "Writing");

    // Schreibe die NDEF-Message auf den Tag
    success = ntag2xx_WriteNDEF(params->payload);
    if (success)
    {
        Serial.println("NDEF-Message erfolgreich auf den Tag geschrieben");
        //oledShowMessage("NFC-Tag written");
        //vTaskDelay(1000 / portTICK_PERIOD_MS);
        nfcReaderState = NFC_WRITE_SUCCESS;
        // aktualisieren der Website wenn sich der Status ändert
        sendNfcData();
        pauseBambuMqttTask = false;

        if(params->tagType){
          // TBD: should this be simplified?
          if (updateSpoolTagId(uidString, params->payload) && params->tagType) {
            // Check if weight is over 20g and send to Spoolman
            if (weight > 20) {
              Serial.println("Tag successfully written and weight > 20g - sending weight to Spoolman");

              // Extract spool ID from payload for weight update
              JsonDocument payloadDoc;
              DeserializationError error = deserializeJson(payloadDoc, params->payload);

              if (!error && payloadDoc["sm_id"].is<String>()) {
                String spoolId = payloadDoc["sm_id"].as<String>();
                if (spoolId != "") {
                  Serial.printf("Updating spool %s with weight %dg\n", spoolId.c_str(), weight);
                  updateSpoolWeight(spoolId, weight);
                } else {
                  Serial.println("No valid spool ID found for weight update");
                }
              } else {
                Serial.println("Error parsing payload for spool ID extraction");
              }

              payloadDoc.clear();
            } else {
              Serial.printf("Weight %dg is not above 20g threshold - skipping weight update\n", weight);
            }
          }else{
            // Potentially handle errors
          }
        }else{
          oledShowProgressBar(1, 1, "Write Tag", "Done!");
        }

        // CRITICAL: Properly stabilize NFC interface after write operation
        Serial.println();
        Serial.println("=== POST-WRITE NFC STABILIZATION ===");

        // Wait for tag operations to complete
        vTaskDelay(500 / portTICK_PERIOD_MS);
        
        // Test tag presence and remove detection
        uint8_t uid[] = { 0, 0, 0, 0, 0, 0, 0 };
        uint8_t uidLength;
        int tagRemovalChecks = 0;
        
        Serial.println("Warte bis Tag entfernt wird...");

        // Monitor tag presence
        while (tagRemovalChecks < 10) {
          yield();
          esp_task_wdt_reset();

          bool tagPresent = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 500);

          if (!tagPresent) {
            Serial.println("✓ Tag wurde entfernt - NFC bereit für nächsten Scan");
            break;
          }

          tagRemovalChecks++;
          Serial.print("Tag noch vorhanden (");
          Serial.print(tagRemovalChecks);
          Serial.println("/10)");

          vTaskDelay(500 / portTICK_PERIOD_MS);
        }

        if (tagRemovalChecks >= 10) {
          Serial.println("WARNUNG: Tag wurde nicht entfernt - fahre trotzdem fort");
        }

        // Additional interface stabilization before resuming normal operations
        Serial.println("Stabilisiere NFC-Interface für normale Operationen...");
        vTaskDelay(200 / portTICK_PERIOD_MS);

        // Test if interface is ready for normal scanning
        uint8_t interfaceTestBuffer[4];
        bool interfaceReady = false;

        for (int testAttempt = 0; testAttempt < 3; testAttempt++) {
          // Try a simple interface operation (without requiring tag presence)
          Serial.print("Interface readiness test ");
          Serial.print(testAttempt + 1);
          Serial.print("/3... ");

          // Use a safe read operation that doesn't depend on tag presence
          // This tests if the PN532 chip itself is responsive
          uint32_t versiondata = nfc.getFirmwareVersion();
          if (versiondata != 0) {
            Serial.println("✓");
            interfaceReady = true;
            break;
          } else {
            Serial.println("❌");
            vTaskDelay(100 / portTICK_PERIOD_MS);
          }
        }

        if (!interfaceReady) {
          Serial.println("WARNUNG: NFC-Interface reagiert nicht - könnte normale Scans beeinträchtigen");
        } else {
          Serial.println("✓ NFC-Interface ist bereit für normale Scans");
        }

        Serial.println("=========================================");

        vTaskResume(RfidReaderTask);
        vTaskDelay(500 / portTICK_PERIOD_MS);
    }
    else
    {
        Serial.println("Fehler beim Schreiben der NDEF-Message auf den Tag");
        oledShowIcon("failed");
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        nfcReaderState = NFC_WRITE_ERROR;
    }
  }
  else
  {
    Serial.println("Fehler: Kein Tag zu schreiben gefunden.");
    oledShowProgressBar(1, 1, "Failure!", "No tag found");
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    nfcReaderState = NFC_IDLE;
  }
  
  sendWriteResult(nullptr, success);
  sendNfcData();

  // Only reset the write protection flag - reading task was never suspended
  nfcWriteInProgress = false; // Re-enable high-level tag operations
  pauseBambuMqttTask = false;

  free(params->payload);
  delete params;

  vTaskDelete(NULL);
}

void startWriteJsonToTag(const bool isSpoolTag, const char* payload) {
  // Optimize JSON to ensure sm_id is first key for fast-path detection
  String optimizedPayload = optimizeJsonForFastPath(payload);
  
  NfcWriteParameterType* parameters = new NfcWriteParameterType();
  parameters->tagType = isSpoolTag;
  parameters->payload = strdup(optimizedPayload.c_str()); // Use optimized payload
  
  // Task nicht mehrfach starten
  if (nfcReaderState == NFC_IDLE || nfcReaderState == NFC_READ_ERROR || nfcReaderState == NFC_READ_SUCCESS) {
    oledShowProgressBar(0, 1, "Write Tag", "Place tag now");
    // Erstelle die Task
    xTaskCreate(
        writeJsonToTag,        // Task-Funktion
        "WriteJsonToTagTask",       // Task-Name
        5115,                        // Stackgröße in Bytes
        (void*)parameters,         // Parameter
        rfidWriteTaskPrio,           // Priorität
        NULL                         // Task-Handle (nicht benötigt)
    );
  }else{
    oledShowProgressBar(0, 1, "FAILURE", "NFC busy!");
    // TBD: Add proper error handling (website)
  }
}

// Safe tag detection with manual retry logic and short timeouts
bool safeTagDetection(uint8_t* uid, uint8_t* uidLength) {
    PROFILE_FUNCTION();
    const int MAX_ATTEMPTS = 1;    // Reduced from 3 to 1 since we are in a loop
    const int SHORT_TIMEOUT = 100; // Very short timeout to prevent hanging
    
    for (int attempt = 0; attempt < MAX_ATTEMPTS; attempt++) {
        // Watchdog reset on each attempt
        esp_task_wdt_reset();
        // yield();
        vTaskDelay(pdMS_TO_TICKS(1));

        
        // Use short timeout to avoid blocking
        // unsigned long start = millis();
        bool success;
        {
          PROFILE_SCOPE("nfc.readPassiveTargetID");
          unsigned long startNfc = millis();
          success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, uidLength, SHORT_TIMEOUT);
          unsigned long duration = millis() - startNfc;
          if (duration > SHORT_TIMEOUT + 50) {
             Serial.printf("[PERF_DEBUG] nfc.readPassiveTargetID took %lu ms (timeout: %d ms)\n", duration, SHORT_TIMEOUT);
             // If timeout exceeded significantly, likely bus issue
             recoverI2C();
          }
        }
        // unsigned long duration = millis() - start;
        // if(duration > 150) Serial.printf("[PERF_DEBUG] nfc.readPassiveTargetID took %lu ms\n", duration);
        
        if (success) {
            Serial.printf("✓ Tag detected on attempt %d with %dms timeout\n", attempt + 1, SHORT_TIMEOUT);
            return true;
        }
        
        // Short pause between attempts
        vTaskDelay(pdMS_TO_TICKS(25));
        
        // Refresh RF field after failed attempt (but not on last attempt)
        if (attempt < MAX_ATTEMPTS - 1) {
            nfc.SAMConfig();
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    
    return false;
}

void scanRfidTask(void * parameter) {
  Serial.println("RFID Task gestartet");
  for(;;) {
    Wire.setTimeOut(50); // Set reasonable timeout in loop

    PROFILE_SCOPE("scanRfidTask Loop");
    // unsigned long start = millis();
    // Regular watchdog reset
    esp_task_wdt_reset();
    yield();
    
    // Skip scanning during write operations, but keep NFC interface active
    if (nfcReaderState != NFC_WRITING && !nfcWriteInProgress && !nfcReadingTaskSuspendRequest && !booting)
    {
      nfcReadingTaskSuspendState = false;
      yield();

      uint8_t success;
      uint8_t uid[] = { 0, 0, 0, 0, 0, 0, 0 };  // Buffer to store the returned UID
      uint8_t uidLength;

      // Use safe tag detection instead of blocking readPassiveTargetID
      success = safeTagDetection(uid, &uidLength);

      foundNfcTag(nullptr, success);
      
      // Reset activeSpoolId immediately when no tag is detected to prevent stale autoSet
      if (!success) {
        activeSpoolId = "";
      }
      
      // As long as there is still a tag on the reader, do not try to read it again
      if (success && nfcReaderState == NFC_IDLE)
      {
        // Set the current tag as not processed
        tagProcessed = false;

        // Display some basic information about the card
        Serial.println("Found an ISO14443A card");

        nfcReaderState = NFC_READING;

        oledShowProgressBar(0, octoEnabled?5:4, "Reading", "Detecting tag");

        // Reduced stabilization time for better responsiveness
        Serial.println("Tag detected, minimal stabilization...");
        vTaskDelay(200 / portTICK_PERIOD_MS); // Reduced from 1000ms to 200ms

        // create Tag UID string
        String uidString = "";
        for (uint8_t i = 0; i < uidLength; i++) {
          //TBD: Rework to remove all the string operations
          uidString += String(uid[i], HEX);
          if (i < uidLength - 1) {
              uidString += ":"; // Optional: Trennzeichen hinzufügen
          }
        }
        
        if (uidLength == 7)
        {
          // Try fast-path detection first for known spools
          if (quickSpoolIdCheck(uidString)) {
              Serial.println("✓ FAST-PATH: Tag processed quickly, skipping full read");
              pauseBambuMqttTask = false;
              // Set reader back to idle for next scan
              nfcReaderState = NFC_READ_SUCCESS;
              delay(500); // Small delay before next scan
              continue; // Skip full tag reading and continue scan loop
          }

          Serial.println("Continuing with full tag read after fast-path check");

          unsigned long readStart = millis();
          uint16_t tagSize = readTagSize();
          unsigned long readDuration = millis() - readStart;
          if(readDuration > 50) Serial.printf("[PERF_DEBUG] readTagSize took %lu ms\n", readDuration);

          if(tagSize > 0)
          {
            // Create a buffer depending on the size of the tag
            uint8_t* data = (uint8_t*)malloc(tagSize);
            memset(data, 0, tagSize);

            // We probably have an NTAG2xx card (though it could be Ultralight as well)
            Serial.println("Seems to be an NTAG2xx tag (7 byte UID)");
            Serial.print("Tag size: ");
            Serial.print(tagSize);
            Serial.println(" bytes");
            
            uint8_t numPages = readTagSize()/4;
            
            unsigned long loopStart = millis();
            for (uint8_t i = 4; i < 4+numPages; i++) {
              
              if (!robustPageRead(i, data+(i-4) * 4))
              {
                Serial.printf("Failed to read page %d after retries, stopping\n", i);
                break; // Stop if reading fails after retries
              }
             
              // Check for NDEF message end
              if (data[(i - 4) * 4] == 0xFE) 
              {
                Serial.println("Found NDEF message end marker");
                break; // End of NDEF message
              }

              yield();
              esp_task_wdt_reset();
              // Reduced delay for faster reading
              vTaskDelay(pdMS_TO_TICKS(2)); // Reduced from 5ms to 2ms
            }
            unsigned long loopDuration = millis() - loopStart;
            if(loopDuration > 100) Serial.printf("[PERF_DEBUG] Full tag read loop took %lu ms\n", loopDuration);
            
            Serial.println("Tag reading completed, starting NDEF decode...");
            
            unsigned long decodeStart = millis();
            if (!decodeNdefAndReturnJson(data, uidString)) 
            {
              oledShowProgressBar(1, 1, "Failure", "Unknown tag");
              nfcReaderState = NFC_READ_ERROR;
            }
            else 
            {
              nfcReaderState = NFC_READ_SUCCESS;
            }
            unsigned long decodeDuration = millis() - decodeStart;
            if(decodeDuration > 50) Serial.printf("[PERF_DEBUG] decodeNdefAndReturnJson took %lu ms\n", decodeDuration);

            free(data);
          }
          else
          {
            oledShowProgressBar(1, 1, "Failure", "Tag read error");
            nfcReaderState = NFC_READ_ERROR;
            // Reset activeSpoolId when tag reading fails to prevent autoSet
            activeSpoolId = "";
            Serial.println("Tag read failed - activeSpoolId reset to prevent autoSet");
          }
        }
        else
        {
          //TBD: Show error here?!
          oledShowProgressBar(1, 1, "Failure", "Unkown tag type");
          Serial.println("This doesn't seem to be an NTAG2xx tag (UUID length != 7 bytes)!");
          // Reset activeSpoolId when tag type is unknown to prevent autoSet
          activeSpoolId = "";
          Serial.println("Unknown tag type - activeSpoolId reset to prevent autoSet");
        }
      }

      if (!success && nfcReaderState != NFC_IDLE && !nfcReadingTaskSuspendRequest)
      {
        nfcReaderState = NFC_IDLE;
        //uidString = "";
        nfcJsonData = "";
        activeSpoolId = "";
        Serial.println("Tag entfernt");
        if (!bambuCredentials.autosend_enable) oledShowWeight(weight);
      }
      // Reset state after successful read when tag is removed
      else if (!success && nfcReaderState == NFC_READ_SUCCESS)
      {
        nfcReaderState = NFC_IDLE;
        Serial.println("Tag nach erfolgreichem Lesen entfernt - bereit für nächsten Tag");
      }

      // Add a pause after successful reading to prevent immediate re-reading
      if (nfcReaderState == NFC_READ_SUCCESS) {
        Serial.println("Tag erfolgreich gelesen - warte 3 Sekunden vor nächstem Scan");
        vTaskDelay(3000 / portTICK_PERIOD_MS); // Reduced from 5 seconds to 3 seconds
      } else {
        // Faster scanning when no tag or idle state
        vTaskDelay(500 / portTICK_PERIOD_MS); // Increased from 150 to 500ms to reduce CPU load
      }

      // aktualisieren der Website wenn sich der Status ändert
      sendNfcData();
    }
    else
    {
      nfcReadingTaskSuspendState = true;
      
      // Different behavior for write protection vs. full suspension
      if (nfcWriteInProgress) {
        // During write: Just pause scanning, don't disable NFC interface
        // Serial.println("NFC Scanning paused during write operation");
        vTaskDelay(100 / portTICK_PERIOD_MS); // Shorter delay during write
      } else {
        // Full suspension requested
        // Serial.println("NFC Reading disabled");
        vTaskDelay(200 / portTICK_PERIOD_MS);
      }
    }
    // yield(); // yield is not enough on single core if priorities are mixed
    vTaskDelay(pdMS_TO_TICKS(10)); // Force context switch to lower priority tasks

    // unsigned long duration = millis() - start;
    // if (duration > 50) {
    //   Serial.printf("[PERF_DEBUG] RfidReaderTask took %lu ms\n", duration);
    // }
  }
}

void startNfc() {
  recoverI2C(); // Recover I2C bus before init
  oledShowProgressBar(5, 7, DISPLAY_BOOT_TEXT, "NFC init");

  // Explicitly initialize I2C pins for ESP32-C3 SuperMini
  // Use setPins before begin
  Wire.setPins(8, 9);
  Wire.begin(8, 9);

  Wire.setTimeOut(50); // Set I2C timeout
  Wire.setClock(100000); // Lower clock speed for stability (100kHz)

  nfc.begin();                                           // Beginne Kommunikation mit RFID Leser
  // Wire.setTimeOut(50); // Redundant if already set, but okay
  delay(1000);
  unsigned long versiondata = nfc.getFirmwareVersion();  // Lese Versionsnummer der Firmware aus
  if (! versiondata) {                                   // Wenn keine Antwort kommt
    Serial.println("Kann kein RFID Board finden !");            // Sende Text "Kann kein..." an seriellen Monitor
    oledShowMessage("No RFID Board found");
    vTaskDelay(2000 / portTICK_PERIOD_MS);
  }
  else {
    Serial.print("Chip PN5 gefunden"); Serial.println((versiondata >> 24) & 0xFF, HEX); // Sende Text und Versionsinfos an seriellen
    Serial.print("Firmware ver. "); Serial.print((versiondata >> 16) & 0xFF, DEC);      // Monitor, wenn Antwort vom Board kommt
    Serial.print('.'); Serial.println((versiondata >> 8) & 0xFF, DEC);                  // 

    nfc.SAMConfig();
    // Do NOT set 400kHz, keep 100kHz
    // Wire.setClock(400000);

    // Set the max number of retry attempts to read from a card
    // This prevents us from waiting forever for a card, which is
    // the default behaviour of the PN532.
    //nfc.setPassiveActivationRetries(0x7F);
    //nfc.setPassiveActivationRetries(0xFF);

    BaseType_t result = xTaskCreatePinnedToCore(
      scanRfidTask, /* Function to implement the task */
      "RfidReader", /* Name of the task */
      5115,  /* Stack size in words */
      NULL,  /* Task input parameter */
      rfidTaskPrio,  /* Priority of the task */
      &RfidReaderTask,  /* Task handle. */
      rfidTaskCore); /* Core where the task should run */

      if (result != pdPASS) {
        Serial.println("Fehler beim Erstellen des RFID Tasks");
    } else {
        Serial.println("RFID Task erfolgreich erstellt");
    }
  }
}