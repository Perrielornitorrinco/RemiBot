#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <SD.h>
#include <SPI.h>
#include <driver/i2s.h>
#include <MFRC522.h>
#include <SoftwareSerial.h>

////////////////////////////////////////////////////////////////////////////
///////////////////////// PARAMETROS A CONFIGURAR //////////////////////////
////////////////////////////////////////////////////////////////////////////

// Configuración WiFi y Telegram
#define WIFI_SSID "nombre"
#define WIFI_PASSWORD "contrasenha"
#define BOT_TOKEN "numerobot:letrasbot"

////////////////////////////////////////////////////////////////////////////
/////////////////// CONFIGURAR SI SE CAMBIAN LOS PINES /////////////////////
////////////////////////////////////////////////////////////////////////////

// Configuración de los pines del ESP32-C3-Mini
// Configuración de la tarjeta SD
#define SD_CS 10
#define SPI_MOSI 6
#define SPI_MISO 5
#define SPI_SCK 4

// Configuración del micrófono I2S
#define I2S_WS 7
#define I2S_SD 2
#define I2S_SCK 3
#define I2S_PORT I2S_NUM_0

// Configuración RFID
#define RFID_SS_PIN 8
#define RFID_RST_PIN 9

// Led PIN
const int ledPin = 21;

////////////////////////////////////////////////////////////////////////////
// FIN PINES
////////////////////////////////////////////////////////////////////////////


// Variables para el debounce de RFID
unsigned long lastCardDetectionTime = 0;
const unsigned long CARD_DEBOUNCE_TIME = 500; // 500ms para confirmar retirada
bool cardPresent = false;
byte lastCardUID[7] = {0};
byte lastCardSize = 0;

// Configuración del botón
#define BUTTON_PIN 20
volatile bool buttonPressed = false;
volatile bool buttonStateChanged = false;

// Configuración de audio
#define SAMPLE_RATE 16000
#define BITS_PER_SAMPLE 16
#define NUM_CHANNELS 1
#define BUFFER_LEN 1024  // Buffer más grande para mejor rendimiento

// Estructura para asociar UID de tarjeta con chat ID de Telegram
struct RFIDUser {
  byte uid[7];        // UID de la tarjeta RFID
  String chatID;      // Chat ID de Telegram asociado
  String userName;    // Nombre del usuario (opcional)
};

// Usuarios autorizados
// MODIFICAR:
// UID de la tarjeta, id del usurio de telegram, nombre de usuario de cara al buzón (el que se quiera)
RFIDUser authorizedUsers[] = {
  {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, "0000000000", "Nombre de usuario 1"}, 
  {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, "0000000000", "Nombre de usuario 2"}
};
const int numUsers = sizeof(authorizedUsers) / sizeof(authorizedUsers[0]);
unsigned long botLastScan;
const unsigned long botMTBS = 1000;

WiFiClientSecure secured_client;
UniversalTelegramBot bot(BOT_TOKEN, secured_client);
MFRC522 mfrc522(RFID_SS_PIN, RFID_RST_PIN);
SoftwareSerial mySerialPrinter(0, 1); // Printer communication

// Variables para el audio
File audioFile;
uint32_t totalBytes = 0;
const char* filename = "/audio_recorded.wav";
bool isRecording = false;
bool waitingForButton = false;
String currentUserChatID = "";

// Buffer para I2S
int16_t sBuffer[BUFFER_LEN];

// Estructura del encabezado WAV
typedef struct {
  char chunkID[4];         // "RIFF"
  uint32_t chunkSize;      // Tamaño del archivo - 8
  char format[4];          // "WAVE"
  char subchunk1ID[4];     // "fmt "
  uint32_t subchunk1Size;  // 16 para PCM
  uint16_t audioFormat;    // 1 para PCM
  uint16_t numChannels;    // 1 para mono, 2 para stereo
  uint32_t sampleRate;     // 44100, etc.
  uint32_t byteRate;       // sampleRate * numChannels * bitsPerSample/8
  uint16_t blockAlign;     // numChannels * bitsPerSample/8
  uint16_t bitsPerSample;  // 16, etc.
  char subchunk2ID[4];     // "data"
  uint32_t subchunk2Size;  // numSamples * numChannels * bitsPerSample/8
} WavHeader;

void IRAM_ATTR buttonISR() {
  bool currentState = digitalRead(BUTTON_PIN);
  if (currentState == LOW) { // Pulsado (usando INPUT_PULLUP)
    buttonPressed = true;
  } else {
    buttonPressed = false;
  }
  buttonStateChanged = true;
}

void setup() {
  mySerialPrinter.begin(9600);
  Serial.begin(9600);
  pinMode(ledPin, OUTPUT);
  delay(1000); // Espera para estabilizar la conexión serial

  digitalWrite(ledPin, HIGH);
  Serial.println("Iniciando sistema...");
  
  // 1. Inicializar periféricos
  initSDCard();
  initI2S();
  initRFID();
  
  // 2. Configurar botón con interrupción
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), buttonISR, CHANGE);
  
  // 3. Conectar a WiFi
  connectToWiFi();
  
  // 4. Configurar hora (necesario para Telegram)
  configTime(0, 0, "pool.ntp.org");
  
  Serial.println("Sistema listo. Acerca una tarjeta RFID y pulsa el botón para grabar.");

  mySerialPrinter.println();
  mySerialPrinter.println("Hola, soy Remi, tu cartero personal. Estoy aqui para ayudarte a comunicarte con quien quieras.\n Acabo de conectarme a internet y mi dirección IP es: ");
  mySerialPrinter.println(WiFi.localIP());
  mySerialPrinter.println();
  digitalWrite(ledPin, LOW);

}

void loop() {
  // Verificar si hay una tarjeta RFID presente
  checkRFIDPresence();

  // Manejar el estado del botón
  if (buttonStateChanged) {
    buttonStateChanged = false;
    handleButtonPress();
  }

  // Grabar si estamos en modo grabación
  if (isRecording) {
    recordChunk();
    
    // Mostrar estado cada segundo
    static unsigned long lastStatusTime = 0;
    if (millis() - lastStatusTime > 1000) {
      lastStatusTime = millis();
      Serial.printf("Grabando... %d bytes\n", totalBytes);
    }
  }

  // Comprobación de nuevos mensajes
  if (millis() - botLastScan > botMTBS && !isRecording) {
    int numNewMessages = bot.getUpdates(bot.last_message_received + 1);

    //bot.sendMessage("1509923351", "Prueba de mensaje", "");

    while (numNewMessages) {
      Serial.println("Mensajes nuevos recibidos");
      handleNewMessages(numNewMessages);
      numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    }
    
    botLastScan = millis();
    //delay(10000);
  }
  
  delay(10);
}

void handleButtonPress() {
  if (waitingForButton && buttonPressed && !isRecording) {
    // Comenzar grabación
    startRecording();
    waitingForButton = false;
  }
  else if (isRecording && !buttonPressed) {
    // Detener grabación
    stopRecording();
    if (currentUserChatID != "") {
      digitalWrite(ledPin, HIGH);
      sendAudioToUser(currentUserChatID);
    }
    currentUserChatID = ""; // Resetear para la próxima grabación
  }
}

void startRecording() {
  audioFile = SD.open(filename, FILE_WRITE);
  if (!audioFile) {
    Serial.println("Error al crear archivo de audio");
    return;
  }
  
  writeWAVHeader(audioFile, 0); // Cabecera provisional
  totalBytes = 0;
  isRecording = true;
  Serial.println("Comenzando grabación... (Suelte el botón para detener)");
}

void stopRecording() {
  if (isRecording) {
    // Actualizar cabecera WAV con tamaño real
    audioFile.seek(0);
    writeWAVHeader(audioFile, totalBytes);
    audioFile.close();
    isRecording = false;
    
    Serial.print("Total de bytes grabados: ");
    Serial.println(totalBytes);
  }
}

void recordChunk() {
  size_t bytesRead;
  esp_err_t result = i2s_read(I2S_PORT, &sBuffer, BUFFER_LEN * sizeof(int16_t), &bytesRead, portMAX_DELAY);
  
  if (result == ESP_OK && bytesRead > 0 && audioFile) {
    // Convertir bytes a muestras de audio (int16_t)
    size_t numSamples = bytesRead / sizeof(int16_t);
    
    // Amplificar el buffer (ejemplo: 2.0x de ganancia)
    amplifyAudioBuffer((int16_t*)sBuffer, numSamples, 100.0); // Ajusta el valor 2.0 según necesidad

    audioFile.write((uint8_t*)sBuffer, bytesRead);
    totalBytes += bytesRead;

    digitalWrite(ledPin, !digitalRead(ledPin));
    
    // Mostrar progreso cada segundo aproximadamente
    static uint32_t lastPrint = 0;
    if (millis() - lastPrint > 1000) {
      lastPrint = millis();
      Serial.printf("Grabando: %d bytes\n", totalBytes);
    }
  }
}

// Función para buscar un usuario por su UID
String findUserByUID(byte* uid, byte uidSize) {
  for (int i = 0; i < numUsers; i++) {
    if (memcmp(uid, authorizedUsers[i].uid, uidSize) == 0) {
      return authorizedUsers[i].chatID;
    }
  }
  return ""; // Retorna cadena vacía si no encuentra el usuario
}

// Función para inicializar la tarjeta SD
void initSDCard() {
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
  if (!SD.begin(SD_CS)) {
    Serial.println("Error al inicializar la tarjeta SD");
    while (1);
  }
  Serial.println("Tarjeta SD inicializada correctamente");
}

// Función para inicializar el micrófono I2S
void initI2S() {
  const i2s_config_t i2s_config = {
    .mode = i2s_mode_t(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = i2s_bits_per_sample_t(BITS_PER_SAMPLE),
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_STAND_I2S),
    .intr_alloc_flags = 0,
    .dma_buf_count = 8,
    .dma_buf_len = BUFFER_LEN,
    .use_apll = false
  };

  i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);

  const i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = -1,
    .data_in_num = I2S_SD
  };

  i2s_set_pin(I2S_PORT, &pin_config);
  Serial.println("Micrófono I2S inicializado correctamente");
}

// Función para inicializar el lector RFID
void initRFID() {
  SPI.begin(); 
  mfrc522.PCD_Init();
  Serial.println("Lector RFID inicializado");
}

// Función para escribir el encabezado WAV
void writeWAVHeader(File file, uint32_t dataSize) {
  WavHeader wavHeader;
  
  strncpy(wavHeader.chunkID, "RIFF", 4);
  wavHeader.chunkSize = dataSize + 36;
  strncpy(wavHeader.format, "WAVE", 4);
  strncpy(wavHeader.subchunk1ID, "fmt ", 4);
  wavHeader.subchunk1Size = 16;
  wavHeader.audioFormat = 1;
  wavHeader.numChannels = NUM_CHANNELS;
  wavHeader.sampleRate = SAMPLE_RATE;
  wavHeader.byteRate = SAMPLE_RATE * NUM_CHANNELS * BITS_PER_SAMPLE / 8;
  wavHeader.blockAlign = NUM_CHANNELS * BITS_PER_SAMPLE / 8;
  wavHeader.bitsPerSample = BITS_PER_SAMPLE;
  strncpy(wavHeader.subchunk2ID, "data", 4);
  wavHeader.subchunk2Size = dataSize;
  
  file.write((uint8_t*)&wavHeader, sizeof(WavHeader));
}

// Función para conectar a WiFi
void connectToWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  secured_client.setCACert(TELEGRAM_CERTIFICATE_ROOT);
  
  Serial.print("Conectando a WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConectado!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

// Funciones auxiliares para el envío del archivo
bool isMoreDataAvailable() {
  return audioFile.available();
}

byte getNextByte() {
  return audioFile.read();
}

// Función para enviar audio a un usuario específico
void sendAudioToUser(String chatID) {
  if (chatID == "") return;

  int maxRetries = 5;
  int retryDelay = 2000;
  bool success = false;

  audioFile = SD.open(filename);
  if (!audioFile) {
    Serial.println("Error al abrir el archivo WAV");
    return;
  }
  
  Serial.print("Enviando audio (");
  Serial.print(audioFile.size());
  Serial.println(" bytes)");

  for (int attempt = 1; attempt <= maxRetries; attempt++) {
    audioFile.seek(0);
    
    Serial.print("Intento ");
    Serial.print(attempt);
    Serial.println(" de envío");

    String response = bot.sendDocumentByBinary(
      chatID, 
      "audio/wav", 
      audioFile.size(),
      isMoreDataAvailable, 
      getNextByte, 
      nullptr, 
      nullptr, 
      "audio_recorded.wav"
    );

    if (response.indexOf("\"ok\":true") != -1) {
      success = true;
      Serial.println("Audio enviado con éxito!");
      digitalWrite(ledPin, LOW);
      break;
    } else {
      Serial.println("Error en el envío, reintentando...");
      if (attempt < maxRetries) {
        delay(retryDelay);
      }
    }
  }

  audioFile.close();

  if (!success) {
    Serial.println("Error: No se pudo enviar el audio después de varios intentos");
  }
}

void checkRFIDPresence() {
  // Verifica si hay una tarjeta nueva presente
  if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
    if (!isRecording) { //!waitingForButton && 
      Serial.println("Tarjeta detectada");
      // Buscar el usuario correspondiente
      currentUserChatID = findUserByUID(mfrc522.uid.uidByte, mfrc522.uid.size);
      
      if (currentUserChatID != "") {
        waitingForButton = true;
        digitalWrite(ledPin, HIGH);
        Serial.println("Usuario autorizado. Pulse el botón para grabar.");
        cardPresent = true; // Marcar que hay una tarjeta presente
      } else {
        digitalWrite(ledPin, LOW);
        Serial.println("Usuario no autorizado");
        cardPresent = false;
      }
    }
    mfrc522.PICC_HaltA(); // Detener comunicación con la tarjeta
  }

  // Verificación adicional para tarjetas ya presentes (no nuevas)
  if (cardPresent) {
    byte bufferATQA[2];
    byte bufferSize = sizeof(bufferATQA);

    // Intenta "despertar" la tarjeta para ver si sigue ahí
    if (mfrc522.PICC_WakeupA(bufferATQA, &bufferSize) == MFRC522::STATUS_OK) {
        digitalWrite(ledPin, HIGH); // LED ON: tarjeta presente
    } else {
        digitalWrite(ledPin, LOW);  // LED OFF: tarjeta retirada o no responde
    }
  }
}


void handleNewMessages(int numNewMessages) {
  for (int i = 0; i < numNewMessages; i++) {
    String chat_id = String(bot.messages[i].chat_id);
    String text = bot.messages[i].text;
    String emisor = bot.messages[i].from_name;

    Serial.println (chat_id);
    Serial.println (text);
    Serial.println (emisor);

    // Buscar el chat_id en el array authorizedUsers
    bool authorized = false;
    String userName;
    
    for (int j = 0; j < numUsers; j++) {
      if (chat_id == authorizedUsers[j].chatID) {
        authorized = true;
        userName = authorizedUsers[j].userName;
        break;
      }
    }
    
    if (authorized) {
      //size_t index = std::distance(userChatIDs.begin(), it);
      //String from_name = userNames[index]; // Imprimir nombre del usuario y mensaje recibido
      Serial.print(chat_id);
      Serial.print(" Mensaje de ");
      Serial.print(userName);
      Serial.print(": ");
      String recived_msg = bot.messages[i].text;
      Serial.println(recived_msg); // Responder con el nombre del usuario
      String response = "Hola " + userName + " estoy enviando tu mensaje. Gracias por contar conmigo para esta tarea 📮🤙🏼";
      bot.sendMessage(chat_id, response, "");
      mySerialPrinter.println("Mensaje de "+ userName + ":");
      imprimirMensajeFormateado(recived_msg);
      //mySerialPrinter.println();
      //mySerialPrinter.println(recived_msg);
      //mySerialPrinter.println();
      
      //mySerialPrinter.println(bot.messages[i].text);
      //mySerialPrinter.println();

    } else { // Mensaje de usuario no autorizado
      String response = "Hola " + emisor + "... ¿Quién eres? Irene me ha dicho que no hable con desconocidos... ¡Adiós!";
      bot.sendMessage(chat_id, response, "");
    }
  }
}


void imprimirMensajeFormateado(String mensaje) {
  const int MAX_CARACTERES = 30; // Máximo por línea
  int retardoEntreLineas = 900;  // ms entre líneas
  
  // Limpieza inicial de retornos de carro existentes
  //mensaje.replace("\n", " ");

  while (mensaje.length() > 0) {
    // Encontrar el último espacio dentro del límite
    int corte = MAX_CARACTERES;
    if (mensaje.length() > MAX_CARACTERES) {
      corte = mensaje.lastIndexOf(' ', MAX_CARACTERES);
      if (corte == -1) corte = MAX_CARACTERES; // Si no hay espacios, corta duro
    } else {
      corte = mensaje.length();
    }

    // Extraer y imprimir el segmento
    String linea = mensaje.substring(0, corte);
    linea.trim(); // Eliminar espacios sobrantes
    mySerialPrinter.println(linea);
    mySerialPrinter.println();
    
    // Retardo controlado entre líneas
    delay(retardoEntreLineas);
    
    // Remover el segmento impreso
    mensaje = mensaje.substring(corte);
    mensaje.trim();
  }

  // Línea final vacía para separación
  mySerialPrinter.println();
  delay(retardoEntreLineas * 2); // Retardo adicional post-mensaje
}

void amplifyAudioBuffer(int16_t* buffer, size_t samples, float gain) {
    for (size_t i = 0; i < samples; i++) {
        buffer[i] = constrain((int)(buffer[i] * gain), -32768, 32767); // Evita clipping
    }
}