/*
 * ============================================================================
 *  grabador_vga_28c256.ino
 *
 *  Graba una imagen de 100 x 75 pixeles y 64 colores en una AT28C256, para la
 *  tarjeta de video VGA de Ben Eater.
 *
 *  Hardware: Arduino Nano + 2 x 74HC595  (EL MISMO programador del Proyecto 6,
 *  con los cambios de cable descritos abajo). Monitor serie a 57600 baudios.
 *
 * ----------------------------------------------------------------------------
 *  CABLEADO
 *
 *    D2 -> SER (pin 14 de los 595)   D3 -> SRCLK (pin 11)   D4 -> RCLK (pin 12)
 *
 *    595 #1:  QA..QH -> A0..A7    (EEPROM pines 10, 9, 8, 7, 6, 5, 4, 3)
 *    595 #2:  QA -> A8  (pin 25)      QB -> A9  (pin 24)     QC -> A10 (pin 21)
 *             QD -> A11 (pin 23)      QE -> A12 (pin 2)      QF -> A13 (pin 26)
 *             QG -> A14 (pin 1)  *    QH -> OE  (pin 22)
 *
 *    D5..D12 -> I/O0..I/O7   (EEPROM pines 11, 12, 13, 15, 16, 17, 18, 19)
 *    D13     -> WE           (pin 27)
 *    CE (pin 20) -> GND      GND (pin 14) -> GND      VCC (pin 28) -> +5 V
 *
 *  CAMBIOS RESPECTO AL PROGRAMADOR DE LA AT28C64B:
 *    - A11 (pin 23) y A12 (pin 2): QUITARLOS DE GND y llevarlos a QD y QE.
 *      Si quedan conectados a GND y al 595 a la vez, la salida del 595 entra
 *      en corto contra tierra al ponerse en alto.
 *    - A13 (pin 26): antes sin conectar, ahora a QF. OBLIGATORIO: con A13 a
 *      GND solo se alcanza hasta la direccion 8191 y las filas 64..74 de la
 *      imagen (11 de 75) quedarian imposibles de grabar.
 *    - A14 (pin 1): antes sin conectar. Nunca dejarlo flotando (es una entrada
 *      CMOS real en este chip).
 *
 *  (*) NOTA SOBRE A14:
 *      Para grabar la imagen basta con A14 a GND, porque la imagen vive entre
 *      0x0000 y 0x257F. PERO el comando 's' (desbloqueo SDP) escribe en la
 *      direccion 0x5555, que tiene el bit 14 en 1. Si vas a usar 's', A14
 *      TIENE que estar en QG del segundo 595, no en GND.
 *
 * ----------------------------------------------------------------------------
 *  MAPA DE MEMORIA
 *
 *    direccion = (Y << 7) | X        X = 0..99 (7 bits)   Y = 0..74 (7 bits)
 *
 *    Cada fila ocupa 128 bytes aunque solo 100 sean visibles. Las columnas
 *    100..127 corresponden al contador horizontal 200..263, que cae en el
 *    blanking: el circuito mantiene OE alto ahi y esos bytes NUNCA se leen.
 *    Por eso este sketch NO los graba. Se ahorran 2100 escrituras = 21 s.
 *
 *    Bytes realmente grabados: 100 x 75 = 7500  (unos 75 s a 10 ms por byte).
 *
 * ----------------------------------------------------------------------------
 *  FORMATO DE COLOR (1 byte por pixel):   0 0 R R G G B B
 *
 *    bits 5-4 = rojo, bits 3-2 = verde, bits 1-0 = azul, cada uno de 0 a 3.
 *    Si cableaste el DAC en otro orden, cambia el orden en el script de
 *    Python (constante ORDEN), no aqui.
 *
 * ----------------------------------------------------------------------------
 *  SEGURIDAD DE ESCRITURA (mismo esquema del sketch de diagnostico de P6)
 *
 *    - Estado de reposo: WE en alto y OE de la EEPROM en bajo. Con OE bajo el
 *      chip inhibe por hardware cualquier ciclo de escritura.
 *    - Solo los comandos que graban levantan la bandera 'armed'; al terminar
 *      siempre se vuelve al reposo.
 *    - Durante el arranque del Nano el bootloader parpadea D13 (WE) antes de
 *      que corra este programa. Cuando el chip ya este montado en la tarjeta
 *      de video, WE va a +5 V y el Nano se desconecta.
 *    - Mover cables SIEMPRE con el USB desconectado.
 *
 * ----------------------------------------------------------------------------
 *  COMANDOS
 *
 *    ?   ayuda
 *    a   prueba de lineas de direccion A0..A13
 *    d   volcado hexadecimal de las primeras 4 filas
 *    t   patron de prueba: rejilla con los 64 colores + borde blanco
 *    w   recibe 7500 bytes por serie y los graba (protocolo con ACK)
 *    r   devuelve los 7500 bytes leidos, en binario crudo (para verificar)
 *    s   desbloqueo SDP   (OJO: exige A14 en QG, ver nota arriba)
 *    z   borra la zona de imagen a 0x00
 * ============================================================================
 */

#define SHIFT_DATA    2
#define SHIFT_CLK     3
#define SHIFT_LATCH   4
#define EEPROM_D0     5
#define EEPROM_D7    12
#define WRITE_EN     13

#define IMG_W       100
#define IMG_H        75
#define IMG_BYTES   ((unsigned int)IMG_W * IMG_H)   // 7500

const byte ACK = 0x06;

bool armed = false;    // solo true mientras corre un comando que graba

/* -------------------------------------------------------------------------
 *  Bus de datos por manipulacion directa de puertos
 *
 *  D5,D6,D7   = PD5,PD6,PD7  -> I/O0, I/O1, I/O2
 *  D8..D12    = PB0..PB4     -> I/O3..I/O7
 *  D13 (PB5)  = WE           -> las mascaras lo preservan
 * ------------------------------------------------------------------------- */

static inline void dataBusInput() {
  DDRD  &= ~0xE0;
  DDRB  &= ~0x1F;
  PORTD &= ~0xE0;          // sin pull-ups
  PORTB &= ~0x1F;
}

static inline void dataBusOutput() {
  DDRD |= 0xE0;
  DDRB |= 0x1F;
}

static inline void dataBusWrite(byte d) {
  PORTD = (PORTD & ~0xE0) | ((d & 0x07) << 5);
  PORTB = (PORTB & ~0x1F) | ((d >> 3) & 0x1F);
}

/* -------------------------------------------------------------------------
 *  Direccionamiento
 *
 *  El byte alto se envia primero y, por la cascada, aterriza en el 595 #2.
 *  Con MSBFIRST el bit 7 cae en QH y el bit 0 en QA:
 *
 *    bit   7    6    5    4    3    2    1    0
 *    595   QH   QG   QF   QE   QD   QC   QB   QA
 *    señal OE  A14  A13  A12  A11  A10   A9   A8
 *
 *  15 lineas de direccion + OE = 16 bits exactos. Por eso esta funcion es
 *  identica a la del programador de la 28C64B: solo cambia el tipo a
 *  unsigned int, para que 0x7FFF no se lea como negativo.
 * ------------------------------------------------------------------------- */

void setAddress(unsigned int address, bool outputEnable) {
  shiftOut(SHIFT_DATA, SHIFT_CLK, MSBFIRST,
           (byte)((address >> 8) | (outputEnable ? 0x00 : 0x80)));
  shiftOut(SHIFT_DATA, SHIFT_CLK, MSBFIRST, (byte)address);
  digitalWrite(SHIFT_LATCH, LOW);
  digitalWrite(SHIFT_LATCH, HIGH);
  digitalWrite(SHIFT_LATCH, LOW);
}

// Version rapida, solo para la secuencia SDP, que exige escrituras
// consecutivas dentro de t_BLC (unos 150 us). shiftOut + digitalWrite
// tardan ~130 us por direccion y quedan al limite; esto tarda ~4 us.
static void fastShiftByte(byte v) {
  for (byte i = 0; i < 8; i++) {
    if (v & 0x80) PORTD |= _BV(PD2); else PORTD &= ~_BV(PD2);
    PORTD |=  _BV(PD3);
    PORTD &= ~_BV(PD3);
    v <<= 1;
  }
}

static void fastSetAddress(unsigned int address, bool outputEnable) {
  fastShiftByte((byte)((address >> 8) | (outputEnable ? 0x00 : 0x80)));
  fastShiftByte((byte)address);
  PORTD |=  _BV(PD4);
  PORTD &= ~_BV(PD4);
}

static inline unsigned int pixelAddr(byte x, byte y) {
  return ((unsigned int)y << 7) | x;
}

/* -------------------------------------------------------------------------
 *  Lectura, escritura y reposo
 * ------------------------------------------------------------------------- */

void restState() {
  digitalWrite(WRITE_EN, HIGH);
  dataBusInput();
  setAddress(0, true);          // OE bajo: escritura inhibida por hardware
}

byte readEEPROM(unsigned int address) {
  dataBusInput();
  setAddress(address, true);
  delayMicroseconds(2);         // > t_ACC del chip
  byte data = 0;
  for (int pin = EEPROM_D7; pin >= EEPROM_D0; pin--)
    data = (data << 1) | digitalRead(pin);
  return data;
}

void writeEEPROM(unsigned int address, byte data) {
  if (!armed) return;           // seguro: sin confirmar no se escribe nada
  setAddress(address, false);   // OE alto: la escritura queda habilitada
  dataBusOutput();
  dataBusWrite(data);
  digitalWrite(WRITE_EN, LOW);
  delayMicroseconds(1);         // pulso ~1 us (minimo del chip: 100 ns)
  digitalWrite(WRITE_EN, HIGH);
  dataBusInput();
  delay(10);                    // ciclo interno de escritura (t_WC max)
  // Optimizacion posible: en vez de delay(10) fijo, hacer data polling
  // leyendo I/O7 hasta que coincida con el bit 7 escrito. Baja el tiempo
  // tipico a 3-5 ms, pero agrega un modo de falla (bucle infinito si el
  // chip no responde). Para depurar en protoboard conviene el delay fijo.
}

/* -------------------------------------------------------------------------
 *  Utilidades de consola
 * ------------------------------------------------------------------------- */

void printHex(byte b) {
  if (b < 16) Serial.print('0');
  Serial.print(b, HEX);
}

bool confirm(const __FlashStringHelper *msg) {
  Serial.println(msg);
  delay(100);
  while (Serial.available()) Serial.read();   // descarta el salto de linea
  unsigned long t0 = millis();
  while (!Serial.available()) {
    if (millis() - t0 > 15000UL) {
      Serial.println(F("Cancelado (sin respuesta en 15 s)."));
      return false;
    }
  }
  if (Serial.read() != 'Y') {
    Serial.println(F("Cancelado."));
    return false;
  }
  return true;
}

void help() {
  Serial.println();
  Serial.println(F("Grabador VGA AT28C256  (imagen 100x75, 64 colores RRGGBB)"));
  Serial.println(F("  ?  ayuda"));
  Serial.println(F("  a  prueba de lineas de direccion A0..A13"));
  Serial.println(F("  d  volcado hex de las primeras 4 filas"));
  Serial.println(F("  t  patron de prueba (64 colores + borde blanco)"));
  Serial.println(F("  w  recibir y grabar imagen por serie (7500 bytes)"));
  Serial.println(F("  r  devolver los 7500 bytes leidos (binario)"));
  Serial.println(F("  s  desbloqueo SDP  [exige A14 en QG, no en GND]"));
  Serial.println(F("  z  borrar la zona de imagen a 0x00"));
}

/* -------------------------------------------------------------------------
 *  Diagnostico
 * ------------------------------------------------------------------------- */

// Lee la direccion 0 y cada direccion de un solo bit. Si una linea esta
// suelta o en corto, su direccion devuelve lo mismo que otra.
// A14 no se prueba porque va a GND (no es manejable).
void addressLines() {
  Serial.print(F("0x0000 = "));
  printHex(readEEPROM(0));
  Serial.println();
  for (byte k = 0; k < 14; k++) {
    unsigned int a = (unsigned int)1 << k;
    Serial.print(F("A"));
    Serial.print(k);
    Serial.print(F(" (0x"));
    Serial.print(a, HEX);
    Serial.print(F(") = "));
    printHex(readEEPROM(a));
    Serial.println();
  }
}

void dumpHex() {
  for (byte y = 0; y < 4; y++) {
    for (byte base = 0; base < IMG_W; base += 16) {
      char buf[8];
      sprintf(buf, "%04X:", pixelAddr(base, y));
      Serial.print(buf);
      for (byte off = 0; off < 16 && (byte)(base + off) < IMG_W; off++) {
        Serial.print(' ');
        printHex(readEEPROM(pixelAddr(base + off, y)));
      }
      Serial.println();
    }
  }
}

/* -------------------------------------------------------------------------
 *  Patron de prueba
 *
 *  Rejilla de 8 x 8 celdas con los 64 colores (indice = fila*8 + columna)
 *  y un borde blanco de 1 pixel.
 *
 *  Para que sirve: la rejilla muestra los 64 colores a la vez, asi que un
 *  nivel del DAC mal calculado o una resistencia equivocada salta a la vista
 *  como una columna o fila de colores repetidos. El borde verifica que la
 *  ventana visible coincide con el area de imagen (si el blanking esta mal,
 *  el borde se corta o aparecen franjas afuera).
 * ------------------------------------------------------------------------- */

void testPattern() {
  if (!confirm(F("Patron de prueba: SOBRESCRIBE la imagen. Envia Y (mayuscula)."))) return;
  armed = true;
  Serial.println(F("Grabando patron (aprox. 75 s)..."));
  for (byte y = 0; y < IMG_H; y++) {
    for (byte x = 0; x < IMG_W; x++) {
      byte c;
      if (x == 0 || x == IMG_W - 1 || y == 0 || y == IMG_H - 1) {
        c = 0x3F;                                       // blanco maximo
      } else {
        byte col = (byte)(((unsigned int)x * 8) / IMG_W);   // 0..7
        byte row = (byte)(((unsigned int)y * 8) / IMG_H);   // 0..7
        c = (byte)(row * 8 + col);                      // 0..63
      }
      writeEEPROM(pixelAddr(x, y), c);
    }
    Serial.print('.');
  }
  armed = false;
  restState();
  Serial.println();
  Serial.println(F("Patron grabado."));
}

void eraseImage() {
  if (!confirm(F("Borra la zona de imagen a 0x00. Envia Y (mayuscula)."))) return;
  armed = true;
  Serial.println(F("Borrando (aprox. 75 s)..."));
  for (byte y = 0; y < IMG_H; y++) {
    for (byte x = 0; x < IMG_W; x++) writeEEPROM(pixelAddr(x, y), 0x00);
    Serial.print('.');
  }
  armed = false;
  restState();
  Serial.println();
  Serial.println(F("Listo."));
}

/* -------------------------------------------------------------------------
 *  Imagen por puerto serie
 *
 *  Protocolo (el script de Python lo implementa del otro lado):
 *    PC -> 'w'
 *    AR -> "READY\r\n"
 *    repetir 7500 veces:  PC -> 1 byte   |   AR graba y responde 0x06
 *    AR -> "DONE 7500\r\n"
 *
 *  El ACK byte a byte evita desbordar el buffer de 64 bytes del Nano: como
 *  cada escritura tarda 10 ms, el PC no puede ir mas rapido que el chip.
 * ------------------------------------------------------------------------- */

void receiveImage() {
  delay(50);
  while (Serial.available()) Serial.read();   // descarta el salto de linea
  Serial.println(F("READY"));
  armed = true;
  unsigned int count = 0;
  for (byte y = 0; y < IMG_H; y++) {
    for (byte x = 0; x < IMG_W; x++) {
      unsigned long t0 = millis();
      while (!Serial.available()) {
        if (millis() - t0 > 10000UL) {
          armed = false;
          restState();
          Serial.println();
          Serial.print(F("TIMEOUT en el byte "));
          Serial.println(count);
          return;
        }
      }
      writeEEPROM(pixelAddr(x, y), (byte)Serial.read());
      Serial.write(ACK);
      count++;
    }
  }
  armed = false;
  restState();
  Serial.print(F("DONE "));
  Serial.println(count);
}

void dumpImageBinary() {
  Serial.println(F("DATA"));
  delay(10);
  for (byte y = 0; y < IMG_H; y++)
    for (byte x = 0; x < IMG_W; x++)
      Serial.write(readEEPROM(pixelAddr(x, y)));
  Serial.flush();
}

/* -------------------------------------------------------------------------
 *  Desbloqueo del Software Data Protection
 *
 *  Sintoma de un chip bloqueado: el programador no reporta ningun error pero
 *  al releer sale todo FF, igual que con un cable suelto. Si 't' graba y 'd'
 *  sigue mostrando FF, prueba esto ANTES de revisar el cableado.
 *
 *  La secuencia escribe en 0x5555 y 0x2AAA, que usan A13 y A14: con A14 a
 *  GND no se puede ejecutar. Verifica la secuencia en el datasheet de
 *  Microchip antes de confiarte.
 * ------------------------------------------------------------------------- */

static void sdpLoad(unsigned int addr, byte data) {
  fastSetAddress(addr, false);
  dataBusWrite(data);
  PORTB &= ~_BV(PB5);                               // WE bajo
  __asm__ __volatile__("nop\n\tnop\n\tnop\n\t");    // ~190 ns (minimo 100 ns)
  PORTB |=  _BV(PB5);                               // WE alto
}

void disableSDP() {
  if (!confirm(F("SDP: A14 (pin 1) DEBE estar en QG del 595, no en GND. Envia Y."))) return;
  dataBusOutput();
  noInterrupts();
  sdpLoad(0x5555, 0xAA);
  sdpLoad(0x2AAA, 0x55);
  sdpLoad(0x5555, 0x80);
  sdpLoad(0x5555, 0xAA);
  sdpLoad(0x2AAA, 0x55);
  sdpLoad(0x5555, 0x20);
  interrupts();
  delay(20);
  restState();
  Serial.println(F("Secuencia SDP enviada. Prueba 't' y luego 'd'."));
}

/* -------------------------------------------------------------------------
 *  setup / loop
 * ------------------------------------------------------------------------- */

void setup() {
  digitalWrite(WRITE_EN, HIGH);   // alto ANTES de configurarlo como salida
  pinMode(WRITE_EN, OUTPUT);
  pinMode(SHIFT_DATA, OUTPUT);
  pinMode(SHIFT_CLK, OUTPUT);
  pinMode(SHIFT_LATCH, OUTPUT);
  dataBusInput();
  restState();
  Serial.begin(57600);
  help();
}

void loop() {
  if (!Serial.available()) return;
  char c = Serial.read();
  switch (c) {
    case '?': help();            break;
    case 'a': addressLines();    break;
    case 'd': dumpHex();         break;
    case 't': testPattern();     break;
    case 'z': eraseImage();      break;
    case 'w': receiveImage();    break;
    case 'r': dumpImageBinary(); break;
    case 's': disableSDP();      break;
    default : return;            // ignora saltos de linea y otros caracteres
  }
  restState();
}
