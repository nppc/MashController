#ifndef WEB_HANDLERS_H
#define WEB_HANDLERS_H

#include <ESP8266WebServer.h>

extern ESP8266WebServer server;

// Registers every HTTP route. Call once from setup(), after storage.begin()
// (settings must be loaded before WiFi.softAP() runs, which happens
// between storage.begin() and this call in the main .ino).
void webHandlersInit();

#endif
