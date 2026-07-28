// ============================================================
// SmartLamp - Firmware ESP32
// Controle de LED via comandos seriais + leitura de sensor LDR
// ============================================================

// --- Pinos ---
const int ledPin = 15;    // Pino do LED (deve suportar PWM)
const int ldrPin = 2;   // Pino do LDR (deve ser um pino ADC do ESP32)

// --- Variáveis de estado/calibração ---
int ledValue = 10;        // Valor atual do LED (escala 0-100)
int ldrMax   = 4000;      // Valor máximo bruto lido do LDR (calibrar em testes)

// Buffer para montar o comando recebido pela Serial
String serialBuffer = "";

void setup() {
    Serial.begin(9600);
    pinMode(ledPin, OUTPUT);
    pinMode(ldrPin, INPUT);
    Serial.printf("SmartLamp Initialized.\n");
}

// Função loop será executada infinitamente pelo ESP32
void loop() {
    // Lê os caracteres disponíveis na Serial até encontrar '\n',
    // monta o comando completo e envia para processCommand()
    while (Serial.available() > 0) {
        char c = Serial.read();
        if (c == '\n') {
            processCommand(serialBuffer);
            serialBuffer = "";
        } else if (c != '\r') {
            serialBuffer += c;
        }
    }
    // while (1)
    // {
        // int ldrValue = ldrGetValue();
        // Serial.printf("RES GET_LDR %d\n", ldrValue);
    // }
    
}

void processCommand(String command) {
    command.trim();

    if (command.startsWith("SET_LED")) {
        int spaceIndex = command.indexOf(' ');
        if (spaceIndex != -1) {
            int value = command.substring(spaceIndex + 1).toInt();
            ledValue = constrain(value, 0, 100); // valor recebido em escala 0-100
            ledUpdate();
            Serial.printf("RES SET_LED 1\n");
        } else {
            Serial.printf("RES SET_LED 0\n");
        }
    }
    else if (command.startsWith("GET_LED")) {
        Serial.printf("RES GET_LED %d\n", ledValue);
    }
    else if (command.startsWith("GET_LDR")) {
        int ldrValue = ldrGetValue();
        Serial.printf("RES GET_LDR %d\n", ldrValue);
    }
    else {
        Serial.printf("ERR Comando invalido\n");
    }
}

// Função para atualizar o valor do LED
void ledUpdate() {
    // Converte de 0-100 (escala do comando) para 0-255 (escala do PWM)
    int pwmValue = map(ledValue, 0, 100, 0, 255);
    pwmValue = constrain(pwmValue, 0, 255);
    analogWrite(ledPin, pwmValue);
}

// Função para ler o valor do LDR
int ldrGetValue() {
    int rawValue = analogRead(ldrPin);

    // Normaliza para 0-100 usando o valor máximo calibrado (ldrMax)
    // Para calibrar: aponte a lanterna do celular para o sensor,
    // veja o maior valor bruto retornado por analogRead() e
    // atualize a constante ldrMax no topo do arquivo.
    int normalized = map(rawValue, 0, ldrMax, 0, 100);
    normalized = constrain(normalized, 0, 100);

    return normalized;
}
