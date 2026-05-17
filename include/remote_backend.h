#pragma once

#include <Arduino.h>

using BackendStatusBuilder = String (*)();
using BackendCommandHandler = void (*)(const String& command, const String& requestId);

void backendBegin(BackendStatusBuilder statusBuilder, BackendCommandHandler commandHandler);
void backendLoop();
void backendPublishStatus(bool force = false);
void backendResetRetryTimer();
bool backendIsEnabled();
bool backendIsConnected();
void backendSendCommandResult(const String& requestId,
                              const String& command,
                              bool ok,
                              const String& detail);
