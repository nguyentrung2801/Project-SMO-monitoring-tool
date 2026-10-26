#include "gateway.h"
void setup()
{
    Serial.begin(115200);
    gatewayBegin();
    webBegin();
}
void loop()
{
    gatewayLoop();
    webLoop();
    delay(1);
}
