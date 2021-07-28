/*********************************************************************
 Adafruit invests time and resources providing this open source code,
 please support Adafruit and open-source hardware by purchasing
 products from Adafruit!

 MIT license, check LICENSE for more information
 Copyright (c) 2019 Ha Thach for Adafruit Industries
 All text above, and the splash screen below must be included in
 any redistribution
*********************************************************************/

/* This example demo how to expose on-board external Flash as USB Mass Storage.
 * Following library is required
 *   - Adafruit_SPIFlash https://github.com/adafruit/Adafruit_SPIFlash
 *   - SdFat https://github.com/adafruit/SdFat
 *
 * Note: Adafruit fork of SdFat enabled ENABLE_EXTENDED_TRANSFER_CLASS and FAT12_SUPPORT
 * in SdFatConfig.h, which is needed to run SdFat on external flash. You can use original
 * SdFat library and manually change those macros
 */

#include "SPI.h"
#include "SdFat.h"
#include "Adafruit_SPIFlash.h"
#include "Adafruit_TinyUSB.h"

#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>

// check if secrets.h is includable, if not please
// create one with SSDI & PASSWORD macro as following example:
// #define SECRET_SSID      "your-ssid"
// #define SECRET_PASSWORD  "your-password"
#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #error "Please create secrets.h with SSID & PASSWORD defined"
#endif

// Debug with FTDI (Serial0) or USBCDC (Serial)
#define DBG_SERIAL Serial

// ESP32 use same flash device that store code.
// Therefore there is no need to specify the SPI and SS
Adafruit_FlashTransport_ESP32 flashTransport;
Adafruit_SPIFlash flash(&flashTransport);

// file system object from SdFat
FatFileSystem fatfs;

//FatFile root;
//FatFile file;

// USB Mass Storage object
Adafruit_USBD_MSC usb_msc;

bool fs_formatted;  // Check if flash is formatted
bool fs_changed;    // Set to true when PC write to flash


const char* host = "esp32fs";
WebServer server(80);
//holds the current upload
File fsUploadFile;

//--------------------------------------------------------------------+
// Setup
//--------------------------------------------------------------------+

void setupMassStorage(void)
{
  flash.begin();

  // Set disk vendor id, product id and revision with string up to 8, 16, 4 characters respectively
  usb_msc.setID("Adafruit", "External Flash", "1.0");

  // Set callback
  usb_msc.setReadWriteCallback(msc_read_cb, msc_write_cb, msc_flush_cb);

  // Set disk size, block size should be 512 regardless of spi flash page size
  usb_msc.setCapacity(flash.size()/512, 512);

  // MSC is ready for read/write
  usb_msc.setUnitReady(true);

  usb_msc.begin();

  // Init file system on the flash
  fs_formatted = fatfs.begin(&flash);

  fs_changed = true; // to print contents initially

  if ( !fs_formatted )
  {
    DBG_SERIAL.println("Failed to init files system, flash may not be formatted");
  }
}

void setupServer(void)
{
  //WIFI INIT
  DBG_SERIAL.printf("Connecting to %s\n", SECRET_SSID);
  if (String(WiFi.SSID()) != String(SECRET_SSID)) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(SECRET_SSID, SECRET_PASSWORD);
  }

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    DBG_SERIAL.print(".");
  }
  DBG_SERIAL.println("");
  DBG_SERIAL.print("Connected! IP address: ");
  DBG_SERIAL.println(WiFi.localIP());

  MDNS.begin(host);
  DBG_SERIAL.print("Open http://");
  DBG_SERIAL.print(host);
  DBG_SERIAL.println(".local/edit to see the file browser");

  //SERVER INIT

  //list directory
  server.on("/list", HTTP_GET, handleFileList);

  //load editor
  server.on("/edit", HTTP_GET, []() {
    if (!handleFileRead("/edit.htm")) {
      server.send(404, "text/plain", "FileNotFound");
    }
  });

  //create file
  server.on("/edit", HTTP_PUT, handleFileCreate);

  //delete file
  server.on("/edit", HTTP_DELETE, handleFileDelete);

  //first callback is called after the request has ended with all parsed arguments
  //second callback handles file uploads at that location
  server.on("/edit", HTTP_POST, []() {
    server.send(200, "text/plain", "");
  }, handleFileUpload);

  //called when the url is not defined here
  //use it to load content from fatfs
  server.onNotFound([]() {
    if (!handleFileRead(server.uri())) {
      server.send(404, "text/plain", "FileNotFound");
    }
  });

  //get heap status, analog input value and all GPIO statuses in one json call
  server.on("/all", HTTP_GET, []() {
    String json = "{";
    json += "\"heap\":" + String(ESP.getFreeHeap());
    json += ", \"analog\":" + String(analogRead(A0));
    json += ", \"gpio\":" + String((uint32_t)(0));
    json += "}";
    server.send(200, "text/json", json);
    json = String();
  });
  server.begin();
  DBG_SERIAL.println("HTTP server started");
}

void setup()
{
  pinMode(LED_BUILTIN, OUTPUT);
  DBG_SERIAL.begin(115200);

  setupMassStorage();

  //  while ( !DBG_SERIAL ) delay(10);   // wait for native usb
  DBG_SERIAL.println("TinyUSB Mass Storage with ESP32 File Browser example");
  DBG_SERIAL.print("JEDEC ID: 0x"); DBG_SERIAL.println(flash.getJEDECID(), HEX);
  DBG_SERIAL.print("Flash size: "); DBG_SERIAL.print(flash.size() / 1024); DBG_SERIAL.println(" KB");

  setupServer();
}

//--------------------------------------------------------------------+
// Handle requets
//--------------------------------------------------------------------+

//format bytes
String formatBytes(size_t bytes) {
  if (bytes < 1024) {
    return String(bytes) + "B";
  } else if (bytes < (1024 * 1024)) {
    return String(bytes / 1024.0) + "KB";
  } else if (bytes < (1024 * 1024 * 1024)) {
    return String(bytes / 1024.0 / 1024.0) + "MB";
  } else {
    return String(bytes / 1024.0 / 1024.0 / 1024.0) + "GB";
  }
}

String getContentType(String filename) {
  if (server.hasArg("download")) {
    return "application/octet-stream";
  } else if (filename.endsWith(".htm")) {
    return "text/html";
  } else if (filename.endsWith(".html")) {
    return "text/html";
  } else if (filename.endsWith(".css")) {
    return "text/css";
  } else if (filename.endsWith(".js")) {
    return "application/javascript";
  } else if (filename.endsWith(".png")) {
    return "image/png";
  } else if (filename.endsWith(".gif")) {
    return "image/gif";
  } else if (filename.endsWith(".jpg")) {
    return "image/jpeg";
  } else if (filename.endsWith(".ico")) {
    return "image/x-icon";
  } else if (filename.endsWith(".xml")) {
    return "text/xml";
  } else if (filename.endsWith(".pdf")) {
    return "application/x-pdf";
  } else if (filename.endsWith(".zip")) {
    return "application/x-zip";
  } else if (filename.endsWith(".gz")) {
    return "application/x-gzip";
  }
  return "text/plain";
}

bool exists(String path){
  bool yes = false;
  File file = fatfs.open(path, O_READ);
  if(file && !file.isDirectory()){
    yes = true;
  }
  file.close();
  return yes;
}

bool handleFileRead(String path) {
  DBG_SERIAL.println("handleFileRead: " + path);
  if (path.endsWith("/")) {
    path += "index.htm";
  }
  String contentType = getContentType(path);
//  String pathWithGz = path + ".gz";
  if ( /*exists(pathWithGz) ||*/ exists(path)) {
//    if (exists(pathWithGz)) {
//      path += ".gz";
//    }
    File file = fatfs.open(path, O_READ);
    server.streamFile(file, contentType);
    file.close();
    return true;
  }
  return false;
}

void handleFileUpload() {
  if (server.uri() != "/edit") {
    return;
  }
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    String filename = upload.filename;
    if (!filename.startsWith("/")) {
      filename = "/" + filename;
    }
    DBG_SERIAL.print("handleFileUpload Name: "); DBG_SERIAL.println(filename);
    fsUploadFile = fatfs.open(filename, O_WRITE);
    filename = String();
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    //DBG_SERIAL.print("handleFileUpload Data: "); DBG_SERIAL.println(upload.currentSize);
    if (fsUploadFile) {
      fsUploadFile.write(upload.buf, upload.currentSize);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (fsUploadFile) {
      fsUploadFile.close();
    }
    DBG_SERIAL.print("handleFileUpload Size: "); DBG_SERIAL.println(upload.totalSize);
  }
}

void handleFileDelete() {
  if (server.args() == 0) {
    return server.send(500, "text/plain", "BAD ARGS");
  }
  String path = server.arg(0);
  DBG_SERIAL.println("handleFileDelete: " + path);
  if (path == "/") {
    return server.send(500, "text/plain", "BAD PATH");
  }
  if (!exists(path)) {
    return server.send(404, "text/plain", "FileNotFound");
  }
  fatfs.remove(path.c_str());
  server.send(200, "text/plain", "");
  path = String();
}

void handleFileCreate() {
  if (server.args() == 0) {
    return server.send(500, "text/plain", "BAD ARGS");
  }
  String path = server.arg(0);
  DBG_SERIAL.println("handleFileCreate: " + path);
  if (path == "/") {
    return server.send(500, "text/plain", "BAD PATH");
  }
  if (exists(path)) {
    return server.send(500, "text/plain", "FILE EXISTS");
  }
  File file = fatfs.open(path, O_WRITE);
  if (file) {
    file.close();
  } else {
    return server.send(500, "text/plain", "CREATE FAILED");
  }
  server.send(200, "text/plain", "");
  path = String();
}

void handleFileList() {
  if (!server.hasArg("dir")) {
    server.send(500, "text/plain", "BAD ARGS");
    return;
  }

  String path = server.arg("dir");
  DBG_SERIAL.println("handleFileList: " + path);

  File root = fatfs.open(path);
  path = String();

  String output = "[";
  if(root.isDirectory()){
      File file = root.openNextFile();
      char fname[256];
      while(file){
          if (output != "[") {
            output += ',';
          }
          output += "{\"type\":\"";
          output += (file.isDirectory()) ? "dir" : "file";
          output += "\",\"name\":\"";
          //output += String(file.path()).substring(1);
          file.getName(fname, sizeof(fname));
          output += fname;
          output += "\"}";
          file = root.openNextFile();
      }
  }
  output += "]";
  server.send(200, "text/json", output);
}

//--------------------------------------------------------------------+
// Loop
//--------------------------------------------------------------------+

void loop()
{
  server.handleClient();
  delay(2);//allow the cpu to switch to other tasks

  if ( fs_changed )
  {
    fs_changed = false;

    // check if host formatted disk
    if (!fs_formatted)
    {
      fs_formatted = fatfs.begin(&flash);
    }

    // skip if still not formatted
    if (!fs_formatted) return;

//    DBG_SERIAL.println("Opening root");

//    if ( !root.open("/") )
//    {
//      DBG_SERIAL.println("open root failed");
//      return;
//    }
//
//    DBG_SERIAL.println("Flash contents:");
//
//    // Open next file in root.
//    // Warning, openNext starts at the current directory position
//    // so a rewind of the directory may be required.
//    while ( file.openNext(&root, O_READ) )
//    {
//      file.printFileSize(&DBG_SERIAL);
//      DBG_SERIAL.write(' ');
//      file.printName(&DBG_SERIAL);
//      if ( file.isDir() )
//      {
//        // Indicate a directory.
//        DBG_SERIAL.write('/');
//      }
//      DBG_SERIAL.println();
//      file.close();
//    }
//
//    root.close();

//    DBG_SERIAL.println();
//    delay(1000);
  }
}

// Callback invoked when received READ10 command.
// Copy disk's data to buffer (up to bufsize) and 
// return number of copied bytes (must be multiple of block size) 
int32_t msc_read_cb (uint32_t lba, void* buffer, uint32_t bufsize)
{
  // Note: SPIFLash Bock API: readBlocks/writeBlocks/syncBlocks
  // already include 4K sector caching internally. We don't need to cache it, yahhhh!!
  return flash.readBlocks(lba, (uint8_t*) buffer, bufsize/512) ? bufsize : -1;
}

// Callback invoked when received WRITE10 command.
// Process data in buffer to disk's storage and 
// return number of written bytes (must be multiple of block size)
int32_t msc_write_cb (uint32_t lba, uint8_t* buffer, uint32_t bufsize)
{
  digitalWrite(LED_BUILTIN, HIGH);

  // Note: SPIFLash Bock API: readBlocks/writeBlocks/syncBlocks
  // already include 4K sector caching internally. We don't need to cache it, yahhhh!!
  return flash.writeBlocks(lba, buffer, bufsize/512) ? bufsize : -1;
}

// Callback invoked when WRITE10 command is completed (status received and accepted by host).
// used to flush any pending cache.
void msc_flush_cb (void)
{
  // sync with flash
  flash.syncBlocks();

  // clear file system's cache to force refresh
  fatfs.cacheClear();

  fs_changed = true;

  digitalWrite(LED_BUILTIN, LOW);
}