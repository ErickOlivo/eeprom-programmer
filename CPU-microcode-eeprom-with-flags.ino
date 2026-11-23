/**
 * Programador de EEPROM para el microcódigo del computador de 8 bits (con banderas Z y C).
 * Base: sketch de Ben Eater (video "Conditional jump instructions", https://youtu.be/Zg1NdPKoosU).
 * Hardware: Arduino Nano/Uno + 2 x 74HC595 + AT28C64B (8 KB) o AT28C16.
 *
 * Cambios respecto al sketch original (el microcódigo es IDÉNTICO):
 *   1. Se agrega verifyEEPROM(): relee los 1024 bytes y compara con lo esperado.
 *   2. printContents() imprime exactamente 'length' bytes desde 'start'.
 *
 * USO: se carga el MISMO sketch y se graba cada chip por separado (≈ 10 s por chip).
 *      Ambos chips quedan con la misma imagen de 1024 bytes (mitad alta y mitad baja).
 *      La diferencia entre U46 y U47 es SOLO el cableado del pin A7 en la computadora:
 *        U46 (byte alto: HLT MI RI RO IO II AI AO)  -> A7 a GND
 *        U47 (byte bajo: EO SU BI OI CE CO J FI)    -> A7 a +5 V
 *
 * Mapa de direcciones (11 bits usados):
 *   A0-A2 = paso T | A3-A6 = opcode (IR) | A7 = selección de byte | A8 = CF | A9 = ZF
 *   A10-A12 de la AT28C64B deben ir a GND en la computadora.
 *
 * AVISO: la grabación ocurre en setup(): cada vez que se abre el monitor serie el
 * Arduino se reinicia y vuelve a grabar. Monitor a 57600 baudios.
 */
#define SHIFT_DATA 2
#define SHIFT_CLK 3
#define SHIFT_LATCH 4
#define EEPROM_D0 5
#define EEPROM_D7 12
#define WRITE_EN 13

#define HLT 0b1000000000000000  // Halt clock
#define MI  0b0100000000000000  // Memory address register in
#define RI  0b0010000000000000  // RAM data in
#define RO  0b0001000000000000  // RAM data out
#define IO  0b0000100000000000  // Instruction register out
#define II  0b0000010000000000  // Instruction register in
#define AI  0b0000001000000000  // A register in
#define AO  0b0000000100000000  // A register out
#define EO  0b0000000010000000  // ALU out
#define SU  0b0000000001000000  // ALU subtract
#define BI  0b0000000000100000  // B register in
#define OI  0b0000000000010000  // Output register in
#define CE  0b0000000000001000  // Program counter enable
#define CO  0b0000000000000100  // Program counter out
#define J   0b0000000000000010  // Jump (program counter in)
#define FI  0b0000000000000001  // Flags in

#define FLAGS_Z0C0 0
#define FLAGS_Z0C1 1
#define FLAGS_Z1C0 2
#define FLAGS_Z1C1 3

#define JC  0b0111
#define JZ  0b1000

#define N_BYTES 1024   // 4 cuadrantes de banderas x 2 mitades x 16 opcodes x 8 pasos

uint16_t UCODE_TEMPLATE[16][8] = {
  { MI|CO,  RO|II|CE,  0,      0,      0,           0, 0, 0 },   // 0000 - NOP
  { MI|CO,  RO|II|CE,  IO|MI,  RO|AI,  0,           0, 0, 0 },   // 0001 - LDA
  { MI|CO,  RO|II|CE,  IO|MI,  RO|BI,  EO|AI|FI,    0, 0, 0 },   // 0010 - ADD
  { MI|CO,  RO|II|CE,  IO|MI,  RO|BI,  EO|AI|SU|FI, 0, 0, 0 },   // 0011 - SUB
  { MI|CO,  RO|II|CE,  IO|MI,  AO|RI,  0,           0, 0, 0 },   // 0100 - STA
  { MI|CO,  RO|II|CE,  IO|AI,  0,      0,           0, 0, 0 },   // 0101 - LDI
  { MI|CO,  RO|II|CE,  IO|J,   0,      0,           0, 0, 0 },   // 0110 - JMP
  { MI|CO,  RO|II|CE,  0,      0,      0,           0, 0, 0 },   // 0111 - JC
  { MI|CO,  RO|II|CE,  0,      0,      0,           0, 0, 0 },   // 1000 - JZ
  { MI|CO,  RO|II|CE,  0,      0,      0,           0, 0, 0 },   // 1001
  { MI|CO,  RO|II|CE,  0,      0,      0,           0, 0, 0 },   // 1010
  { MI|CO,  RO|II|CE,  0,      0,      0,           0, 0, 0 },   // 1011
  { MI|CO,  RO|II|CE,  0,      0,      0,           0, 0, 0 },   // 1100
  { MI|CO,  RO|II|CE,  0,      0,      0,           0, 0, 0 },   // 1101
  { MI|CO,  RO|II|CE,  AO|OI,  0,      0,           0, 0, 0 },   // 1110 - OUT
  { MI|CO,  RO|II|CE,  HLT,    0,      0,           0, 0, 0 },   // 1111 - HLT
};

uint16_t ucode[4][16][8];

void initUCode() {
  // ZF = 0, CF = 0
  memcpy(ucode[FLAGS_Z0C0], UCODE_TEMPLATE, sizeof(UCODE_TEMPLATE));

  // ZF = 0, CF = 1
  memcpy(ucode[FLAGS_Z0C1], UCODE_TEMPLATE, sizeof(UCODE_TEMPLATE));
  ucode[FLAGS_Z0C1][JC][2] = IO|J;

  // ZF = 1, CF = 0
  memcpy(ucode[FLAGS_Z1C0], UCODE_TEMPLATE, sizeof(UCODE_TEMPLATE));
  ucode[FLAGS_Z1C0][JZ][2] = IO|J;

  // ZF = 1, CF = 1
  memcpy(ucode[FLAGS_Z1C1], UCODE_TEMPLATE, sizeof(UCODE_TEMPLATE));
  ucode[FLAGS_Z1C1][JC][2] = IO|J;
  ucode[FLAGS_Z1C1][JZ][2] = IO|J;
}

/*
 * Byte que debe contener la EEPROM en 'address' (se usa al grabar y al verificar).
 */
byte expectedByte(int address) {
  int flags       = (address & 0b1100000000) >> 8;
  int byte_sel    = (address & 0b0010000000) >> 7;
  int instruction = (address & 0b0001111000) >> 3;
  int step        = (address & 0b0000000111);

  if (byte_sel) {
    return ucode[flags][instruction][step];        // byte bajo (U47)
  } else {
    return ucode[flags][instruction][step] >> 8;   // byte alto (U46)
  }
}

/*
 * Output the address bits and outputEnable signal using shift registers.
 */
void setAddress(int address, bool outputEnable) {
  shiftOut(SHIFT_DATA, SHIFT_CLK, MSBFIRST, (address >> 8) | (outputEnable ? 0x00 : 0x80));
  shiftOut(SHIFT_DATA, SHIFT_CLK, MSBFIRST, address);

  digitalWrite(SHIFT_LATCH, LOW);
  digitalWrite(SHIFT_LATCH, HIGH);
  digitalWrite(SHIFT_LATCH, LOW);
}


/*
 * Read a byte from the EEPROM at the specified address.
 */
byte readEEPROM(int address) {
  for (int pin = EEPROM_D0; pin <= EEPROM_D7; pin += 1) {
    pinMode(pin, INPUT);
  }
  setAddress(address, /*outputEnable*/ true);

  byte data = 0;
  for (int pin = EEPROM_D7; pin >= EEPROM_D0; pin -= 1) {
    data = (data << 1) + digitalRead(pin);
  }
  return data;
}


/*
 * Write a byte to the EEPROM at the specified address.
 * Pulso de WE̅ de ~1 us (el mínimo de la AT28C64B es 100 ns) y espera de 10 ms (t_WC máx).
 */
void writeEEPROM(int address, byte data) {
  setAddress(address, /*outputEnable*/ false);
  for (int pin = EEPROM_D0; pin <= EEPROM_D7; pin += 1) {
    pinMode(pin, OUTPUT);
  }

  for (int pin = EEPROM_D0; pin <= EEPROM_D7; pin += 1) {
    digitalWrite(pin, data & 1);
    data = data >> 1;
  }
  digitalWrite(WRITE_EN, LOW);
  delayMicroseconds(1);
  digitalWrite(WRITE_EN, HIGH);
  delay(10);
}


/*
 * Read the contents of the EEPROM and print them to the serial monitor.
 */
void printContents(int start, int length) {
  for (int base = start; base < start + length; base += 16) {
    byte data[16];
    for (int offset = 0; offset <= 15; offset += 1) {
      data[offset] = readEEPROM(base + offset);
    }

    char buf[80];
    sprintf(buf, "%03x:  %02x %02x %02x %02x %02x %02x %02x %02x   %02x %02x %02x %02x %02x %02x %02x %02x",
            base, data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7],
            data[8], data[9], data[10], data[11], data[12], data[13], data[14], data[15]);

    Serial.println(buf);
  }
}


/*
 * Compara lo leído con lo esperado en los N_BYTES grabados y reporta las diferencias.
 */
void verifyEEPROM() {
  int errors = 0;
  for (int address = 0; address < N_BYTES; address += 1) {
    byte expected = expectedByte(address);
    byte actual   = readEEPROM(address);
    if (actual != expected) {
      errors += 1;
      if (errors <= 20) {            // no inundar el monitor serie
        char buf[60];
        sprintf(buf, "ERROR en %03x: esperado %02x, leido %02x", address, expected, actual);
        Serial.println(buf);
      }
    }
  }
  if (errors == 0) {
    Serial.println("Verificacion OK: 1024/1024 bytes coinciden");
  } else {
    Serial.print("Verificacion FALLIDA: bytes con error = ");
    Serial.println(errors);
  }
}


void setup() {
  initUCode();

  pinMode(SHIFT_DATA, OUTPUT);
  pinMode(SHIFT_CLK, OUTPUT);
  pinMode(SHIFT_LATCH, OUTPUT);
  digitalWrite(WRITE_EN, HIGH);
  pinMode(WRITE_EN, OUTPUT);
  Serial.begin(57600);

  // Program data bytes
  Serial.print("Programming EEPROM");

  // Se graban 1024 bytes: 4 cuadrantes de banderas x (mitad alta + mitad baja)
  for (int address = 0; address < N_BYTES; address += 1) {
    writeEEPROM(address, expectedByte(address));

    if (address % 64 == 0) {
      Serial.print(".");
    }
  }

  Serial.println(" done");

  // Read and print out the contents of the EEPROM
  Serial.println("Reading EEPROM");
  printContents(0, N_BYTES);

  Serial.println("Verificando...");
  verifyEEPROM();
}


void loop() {
}
