// ESP32 RS-485 master for split-flap modules (firmware v7)
#include <Arduino.h>

#define RS485_RX 6
#define RS485_TX 7
#define RS485_DE 8

#define RS485_BAUD 9600

// ---------- Low-level send ----------
void rs485Send(const String &msg) {
  digitalWrite(RS485_DE, HIGH);          // transmit mode
  delayMicroseconds(200);
  Serial1.print(msg);
  Serial1.flush();                       // wait until all bytes are shifted out
  delayMicroseconds(500);
  digitalWrite(RS485_DE, LOW);           // back to receive mode
}

// ---------- Helpers (id = 1..255, or -1 for broadcast) ----------
String addr(int id) { return id < 0 ? "m*" : "m" + String(id); }

void showChar(int id, char c)   { rs485Send(addr(id) + "-" + c + "\n"); }
void showIndex(int id, int idx) { rs485Send(addr(id) + "+" + String(idx) + "\n"); }
void homeModule(int id)         { rs485Send(addr(id) + "h\n"); }
void calibrate(int id)          { rs485Send(addr(id) + "c\n"); }
void dumpConfig(int id)         { rs485Send(addr(id) + "d\n"); }
void setHomeOffset(int id, int v) { rs485Send(addr(id) + "o" + String(v) + "\n"); }
void setTotalSteps(int id, int v) { rs485Send(addr(id) + "t" + String(v) + "\n"); }
void nudge(int id, int steps)     { rs485Send(addr(id) + "s" + String(steps) + "\n"); }
void gotoStep(int id, int step)   { rs485Send(addr(id) + "g" + String(step) + "\n"); }
void writeMap(int id, int idx, int pos) { rs485Send(addr(id) + "w" + String(idx) + ":" + String(pos) + "\n"); }
void setModuleId(int id, int newId)     { rs485Send(addr(id) + "i" + String(newId) + "\n"); }
void setAutoHome(int id, bool on)       { rs485Send(addr(id) + "a" + String(on ? 1 : 0) + "\n"); }
void eraseMap(int id)                   { rs485Send(addr(id) + "e\n"); }

// Show a string across consecutive modules: first char -> startId, next -> startId+1, ...
void showText(int startId, const String &text) {
  for (size_t i = 0; i < text.length(); i++) {
    showChar(startId + i, text[i]);
    delay(5);                            // small gap between messages
  }
}

// ---------- USB console ----------
// Type into the Serial Monitor (line ending: Newline):
//   38-B          -> module 38 shows 'B'
//   38+7          -> module 38 shows flap index 7
//   *h            -> home all modules
//   38d           -> dump module 38 config
//   text 1 HELLO  -> modules 1..5 show H,E,L,L,O
//   m38-B         -> raw message, sent as-is
void handleConsoleLine(String line) {
  line.replace("\r", "");
  line.replace("\n", "");
  if (line.length() == 0) return;

  if (line.startsWith("text ")) {
    int sp = line.indexOf(' ', 5);
    if (sp > 0) {
      int startId = line.substring(5, sp).toInt();
      showText(startId, line.substring(sp + 1));
    }
    return;
  }
  if (line[0] == 'm') rs485Send(line + "\n");
  else                rs485Send("m" + line + "\n");
}

void setup() {
  Serial.begin(115200);
  pinMode(RS485_DE, OUTPUT);
  digitalWrite(RS485_DE, LOW);
  Serial1.begin(RS485_BAUD, SERIAL_8N1, RS485_RX, RS485_TX);
  Serial.println("RS485 master ready");
}

void loop() {
  // USB -> bus
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') { handleConsoleLine(line); line = ""; }
    else line += c;
  }

  // Bus -> USB (module replies: calibration result, EEPROM dump)
  while (Serial1.available()) {
    Serial.write(Serial1.read());
  }
}