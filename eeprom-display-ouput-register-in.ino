/*
 * Diagnostico y lectura de EEPROM (AT28C64B / 28C16) con el programador de Ben Eater
 * Arduino Nano + 2 x 74HC595.  Monitor serie a 57600 baudios (cualquier ajuste de fin de linea).
 * Placa en el IDE: Arduino/Genuino Uno (el clon trae el bootloader nuevo).
 *
 * Cableado (igual que el programador de Ben, NO hay que cambiar nada):
 *   D2 = SER (datos seriales)   D3 = SRCLK (reloj de desplazamiento)   D4 = RCLK (latch)
 *   D5..D12 = I/O0..I/O7 de la EEPROM        D13 = WE (pin 27 de la AT28C64B)
 *   Bit 7 del byte alto del 2.o 595 = OE (pin 22).   A11 (pin 23), A12 (pin 2) y CE (pin 20) a GND.
 *
 * SEGURIDAD DE ESCRITURA (por software):
 *   - Estado de reposo: WE (D13) en alto y OE de la EEPROM en bajo. Con OE en bajo el chip
 *     inhibe cualquier ciclo de escritura (datasheet, "Hardware Data Protection").
 *   - Solo los comandos t y g pueden escribir, y piden confirmar con Y. Al terminar vuelven al reposo.
 *   - Limite: durante el arranque del Nano (bootloader) D13 parpadea antes de que este programa
 *     corra; el reposo con OE en bajo queda "recordado" por el 595 tras cualquier comando de lectura.
 *     Despues de apagar y volver a encender, el 595 arranca con valores indefinidos hasta el 1.er comando.
 *
 * Solo se explora 0x000..0x7FF (2 KB), que es la zona usada por la tabla de 7 segmentos.
 *
 * Comandos:
 *   d  Volcado de 0x000..0x7FF en hexadecimal (solo lectura)
 *   v  Verifica contra la tabla esperada del display (solo lectura)
 *   s  Prueba de estabilidad: lee cada direccion 5 veces y compara (solo lectura)
 *   a  Prueba de lineas de direccion: lee 0x000 y cada direccion de un solo bit (solo lectura)
 *   b  Prueba de lineas de datos: cuenta cuantas veces cada bit lee 1 en los 2 KB (solo lectura)
 *   p  Prueba de OE: compara lectura con OE bajo vs OE alto usando pull-ups (solo lectura)
 *   t  Prueba de escritura/lectura con 0x55, 0xAA y dato = direccion (BORRA LA TABLA)
 *   g  Graba la tabla del display de 7 segmentos y la verifica
 */

#define SHIFT_DATA 2
#define SHIFT_CLK 3
#define SHIFT_LATCH 4
#define EEPROM_D0 5
#define EEPROM_D7 12
#define WRITE_EN 13

#define EEPROM_SIZE 2048   // zona usada: A0..A10

const byte digits[10] = {0x7e, 0x30, 0x6d, 0x79, 0x33, 0x5b, 0x5f, 0x70, 0x7f, 0x7b};

bool armed = false;        // solo vale true mientras corren los comandos t o g

void setAddress(int address, bool outputEnable) {
  shiftOut(SHIFT_DATA, SHIFT_CLK, MSBFIRST, (address >> 8) | (outputEnable ? 0x00 : 0x80));
  shiftOut(SHIFT_DATA, SHIFT_CLK, MSBFIRST, address);
  digitalWrite(SHIFT_LATCH, LOW);
  digitalWrite(SHIFT_LATCH, HIGH);
  digitalWrite(SHIFT_LATCH, LOW);
}

// Estado de reposo: WE alto, datos como entradas, OE de la EEPROM en bajo (escritura inhibida)
void restState() {
  digitalWrite(WRITE_EN, HIGH);
  for (int pin = EEPROM_D0; pin <= EEPROM_D7; pin += 1) pinMode(pin, INPUT);
  setAddress(0, true);
}

byte readEEPROM(int address) {
  for (int pin = EEPROM_D0; pin <= EEPROM_D7; pin += 1) pinMode(pin, INPUT);
  setAddress(address, true);
  byte data = 0;
  for (int pin = EEPROM_D7; pin >= EEPROM_D0; pin -= 1) data = (data << 1) + digitalRead(pin);
  return data;
}

void writeEEPROM(int address, byte data) {
  if (!armed) return;            // seguro: sin confirmar no se escribe nada
  setAddress(address, false);
  for (int pin = EEPROM_D0; pin <= EEPROM_D7; pin += 1) pinMode(pin, OUTPUT);
  for (int pin = EEPROM_D0; pin <= EEPROM_D7; pin += 1) {
    digitalWrite(pin, data & 1);
    data = data >> 1;
  }
  digitalWrite(WRITE_EN, LOW);
  delayMicroseconds(1);          // pulso de ~1 us (minimo del chip: 100 ns)
  digitalWrite(WRITE_EN, HIGH);
  delay(10);                     // ciclo interno de escritura (hasta 10 ms)
}

// Valor que debe tener la direccion segun el mapa del display (mismo que el sketch de Ben)
byte expected(int addr) {
  int block = addr >> 8;         // 0..7
  int b = addr & 0xFF;
  if (block < 4) {               // sin signo
    if (block == 0) return digits[b % 10];
    if (block == 1) return digits[(b / 10) % 10];
    if (block == 2) return digits[(b / 100) % 10];
    return 0x00;
  } else {                       // complemento a dos
    int v = (int8_t)b;
    if (block == 4) return digits[abs(v) % 10];
    if (block == 5) return digits[abs(v / 10) % 10];
    if (block == 6) return digits[abs(v / 100) % 10];
    return (v < 0) ? 0x01 : 0x00;
  }
}

void printHex(byte b) {
  if (b < 16) Serial.print('0');
  Serial.print(b, HEX);
}

void dumpEEPROM() {
  for (int base = 0; base < EEPROM_SIZE; base += 16) {
    char buf[8];
    sprintf(buf, "%03x:", base);
    Serial.print(buf);
    for (int off = 0; off < 16; off += 1) {
      Serial.print(' ');
      printHex(readEEPROM(base + off));
    }
    Serial.println();
  }
}

void verifyTable() {
  int bad[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  int shown = 0;
  for (int a = 0; a < EEPROM_SIZE; a += 1) {
    byte got = readEEPROM(a);
    byte exp = expected(a);
    if (got != exp) {
      bad[a >> 8] += 1;
      if (shown < 10) {
        Serial.print("  dir 0x"); Serial.print(a, HEX);
        Serial.print(" leido 0x"); printHex(got);
        Serial.print(" esperado 0x"); printHex(exp);
        Serial.println();
        shown += 1;
      }
    }
  }
  int total = 0;
  for (int k = 0; k < 8; k += 1) {
    Serial.print("Bloque "); Serial.print(k + 1); Serial.print(": errores = "); Serial.println(bad[k]);
    total += bad[k];
  }
  Serial.print("TOTAL de errores: "); Serial.println(total);
  if (total == 0) Serial.println("La tabla coincide con la esperada.");
}

void stabilityTest() {
  int unstable = 0;
  for (int a = 0; a < EEPROM_SIZE; a += 1) {
    byte first = readEEPROM(a);
    for (int n = 0; n < 4; n += 1) {
      if (readEEPROM(a) != first) {
        unstable += 1;
        if (unstable <= 10) { Serial.print("  Lectura inestable en 0x"); Serial.println(a, HEX); }
        break;
      }
    }
  }
  Serial.print("Direcciones inestables: "); Serial.println(unstable);
}

// Lee 0x000 y las direcciones de un solo bit (0x001, 0x002, 0x004 ... 0x400).
// Si una linea de direccion esta suelta o en corto, su direccion devuelve lo mismo que otra.
void addressLines() {
  Serial.print("0x000 = "); printHex(readEEPROM(0)); Serial.println();
  for (int k = 0; k < 11; k += 1) {
    int a = 1 << k;
    Serial.print("A"); Serial.print(k); Serial.print(" (0x"); Serial.print(a, HEX);
    Serial.print(") = "); printHex(readEEPROM(a)); Serial.println();
  }
}

// Cuenta cuantas veces cada linea de datos lee 1 en las 2048 direcciones.
// Un bit con 0 o con 2048 esta pegado (cable, pin o celda).
void bitCount() {
  int ones[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  for (int a = 0; a < EEPROM_SIZE; a += 1) {
    byte v = readEEPROM(a);
    for (int k = 0; k < 8; k += 1) if (v & (1 << k)) ones[k] += 1;
  }
  for (int k = 0; k < 8; k += 1) {
    Serial.print("I/O"); Serial.print(k); Serial.print(": unos = "); Serial.print(ones[k]);
    Serial.print(" de "); Serial.println(EEPROM_SIZE);
  }
}

// Compara la lectura con OE bajo (chip maneja el bus) contra OE alto (chip suelta el bus).
// Los pull-ups internos del Nano llevan a 1 las lineas que nadie maneja. WE queda siempre en alto.
void pullupTest() {
  const int addrs[10] = {0x000, 0x001, 0x002, 0x003, 0x004, 0x010, 0x080, 0x100, 0x400, 0x7FF};
  Serial.println("dir    OE bajo   OE alto");
  int same = 0;
  for (int i = 0; i < 10; i += 1) {
    for (int pin = EEPROM_D0; pin <= EEPROM_D7; pin += 1) pinMode(pin, INPUT_PULLUP);
    setAddress(addrs[i], true);
    delayMicroseconds(50);
    byte lo = 0;
    for (int pin = EEPROM_D7; pin >= EEPROM_D0; pin -= 1) lo = (lo << 1) + digitalRead(pin);
    setAddress(addrs[i], false);
    delayMicroseconds(50);
    byte hi = 0;
    for (int pin = EEPROM_D7; pin >= EEPROM_D0; pin -= 1) hi = (hi << 1) + digitalRead(pin);
    char buf[8];
    sprintf(buf, "%03x", addrs[i]);
    Serial.print(buf); Serial.print("      "); printHex(lo); Serial.print("        "); printHex(hi);
    Serial.println();
    if (lo == hi) same += 1;
  }
  Serial.print("Direcciones donde OE bajo y OE alto leen igual: "); Serial.print(same); Serial.println(" de 10");
}

bool confirmWrite(const char* msg) {
  Serial.println(msg);
  delay(100);
  while (Serial.available()) Serial.read();   // descarta el salto de linea que llega junto al comando
  while (!Serial.available()) {}
  if (Serial.read() != 'Y') { Serial.println("Cancelado."); return false; }
  return true;
}

int fillAndCheck(byte mode) {   // mode: 0 = 0x55, 1 = 0xAA, 2 = dato = direccion
  int errors = 0;
  for (int a = 0; a < EEPROM_SIZE; a += 1) {
    byte val = (mode == 0) ? 0x55 : (mode == 1) ? 0xAA : (byte)(a ^ (a >> 8));
    writeEEPROM(a, val);
  }
  for (int a = 0; a < EEPROM_SIZE; a += 1) {
    byte val = (mode == 0) ? 0x55 : (mode == 1) ? 0xAA : (byte)(a ^ (a >> 8));
    byte got = readEEPROM(a);
    if (got != val) {
      errors += 1;
      if (errors <= 10) {
        Serial.print("  dir 0x"); Serial.print(a, HEX);
        Serial.print(" leido 0x"); printHex(got);
        Serial.print(" escrito 0x"); printHex(val);
        Serial.println();
      }
    }
  }
  return errors;
}

void writeTest() {
  if (!confirmWrite("ATENCION: esto BORRA la tabla. Envia Y (mayuscula) para continuar.")) return;
  armed = true;
  const char* names[3] = {"patron 0x55", "patron 0xAA", "dato = direccion"};
  for (byte m = 0; m < 3; m += 1) {
    Serial.print("Prueba "); Serial.print(names[m]); Serial.println(" ...");
    int e = fillAndCheck(m);
    Serial.print("  errores: "); Serial.println(e);
  }
  armed = false;
  Serial.println("Fin. Usa 'g' para volver a grabar la tabla.");
}

void programTable() {
  if (!confirmWrite("Esto SOBRESCRIBE la tabla del display. Envia Y (mayuscula) para continuar.")) return;
  armed = true;
  Serial.println("Grabando tabla (aprox. 20 s)...");
  for (int a = 0; a < EEPROM_SIZE; a += 1) {
    writeEEPROM(a, expected(a));
    if ((a & 0xFF) == 0xFF) Serial.print('.');
  }
  armed = false;
  Serial.println();
  Serial.println("Verificando...");
  verifyTable();
}

void setup() {
  digitalWrite(WRITE_EN, HIGH);   // WE en alto ANTES de configurarlo como salida (evita escrituras)
  pinMode(WRITE_EN, OUTPUT);
  pinMode(SHIFT_DATA, OUTPUT);
  pinMode(SHIFT_CLK, OUTPUT);
  pinMode(SHIFT_LATCH, OUTPUT);
  restState();                    // OE de la EEPROM en bajo: escritura inhibida
  Serial.begin(57600);
  Serial.println("Comandos: d=volcado  v=verificar  s=estabilidad  a=lineas de direccion  b=lineas de datos  p=prueba OE  t=prueba escritura  g=grabar tabla");
}

void loop() {
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'd') dumpEEPROM();
    else if (c == 'v') verifyTable();
    else if (c == 's') stabilityTest();
    else if (c == 'a') addressLines();
    else if (c == 'b') bitCount();
    else if (c == 'p') pullupTest();
    else if (c == 't') writeTest();
    else if (c == 'g') programTable();
    else return;                  // ignora saltos de linea y otros caracteres
    restState();                  // siempre vuelve al reposo seguro
  }
}
