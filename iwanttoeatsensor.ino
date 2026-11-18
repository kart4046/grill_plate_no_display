#include <OneWire.h>                                // библиотека протокола 1-Wire
#include <DallasTemperature.h>                      // библиотека датчиков DS18B20

#define ENC_CLK        32                           // пин CLK энкодера (выход A)
#define ENC_DT         33                           // пин DT энкодера (выход B)
#define ENC_SW         25                           // пин SW энкодера (кнопка)
#define ENC_DIR        -1                           // направление вращения: если крутится не туда, поставь 1

#define ONE_WIRE_PIN   4                            // пин данных DS18B20 (с подтяжкой 4.7k к 3.3V)

#define LED_RED        26                           // пин красного светодиода (через резистор 330 Ω)
#define LED_GREEN      27                           // пин зелёного светодиода (через резистор 330 Ω)
#define HEAT_PIN       13                           // выход ШИМ для твердотельного реле (SSR)

#define TEMP_MIN       20                           // минимальная уставка температуры, °C (для теста; по заданию 120)
#define TEMP_MAX       35                           // максимальная уставка температуры, °C (для теста; по заданию 220)
#define TEMP_STEP      5                            // шаг изменения уставки, °C
#define TEMP_START     25                           // стартовая уставка при включении, °C (для теста; по заданию 150)
#define TEMP_TOL       2.0                          // допуск «температура в норме», °C (для теста ±2; по заданию ±5)
#define TEMP_HYST      0.5                          // гистерезис зелёного, °C (чтобы светодиод не мигал на границе)

#define PWM_GAIN       10.0                         // мощность в % на каждый градус ошибки (для теста 10; по заданию 2.0)
#define PWM_PERIOD_MS  1000                         // период медленного ШИМ для SSR, мс (окно 1 секунда)

OneWire oneWire(ONE_WIRE_PIN);                      // объект шины 1-Wire на выбранном пине
DallasTemperature sensors(&oneWire);                // объект работы с датчиками температуры

int setPoint = TEMP_START;                          // текущая заданная температура, °C
bool heatingOn = false;                             // нагрев включён или нет (после старта всегда выключен)
float currentTemp = 0.0;                            // последняя измеренная температура, °C
bool sensorOk = false;                              // true, если датчик отвечает и значение корректное
bool redOn = false;                                 // состояние красного светодиода (идёт нагрев)
bool greenOn = false;                               // состояние зелёного светодиода (температура в норме)
float dutyPercent = 0.0;                            // текущая мощность нагрева, 0..100 %
bool heatPinState = false;                          // текущее состояние выхода ШИМ (включён или выключен)
unsigned long pwmWindowStart = 0;                   // время начала текущего окна ШИМ

const int8_t encTable[16] = {0, 1, -1, 0, -1, 0, 0, 1, 1, 0, 0, -1, 0, -1, 1, 0}; // таблица декодирования квадратурного сигнала
uint8_t encState = 0;                               // предыдущее и текущее состояние пинов CLK и DT
int8_t encAccum = 0;                                // накопитель шагов (на один щелчок приходится 4 шага)

void printStatus() {                                // вывод текущего состояния в монитор порта
  if (sensorOk) {                                   // если датчик работает
    Serial.printf("Now: %.1f C | Set: %d C | Heating: %s | PWM: %.0f%% | Red: %s | Green: %s\n", currentTemp, setPoint, heatingOn ? "ON" : "OFF", dutyPercent, redOn ? "ON" : "off", greenOn ? "ON" : "off"); // температура, уставка, нагрев, мощность, светодиоды
  } else {                                          // если датчик не отвечает
    Serial.printf("Now: --- (sensor error) | Set: %d C | Heating: %s | PWM: %.0f%% | Red: %s | Green: %s\n", setPoint, heatingOn ? "ON" : "OFF", dutyPercent, redOn ? "ON" : "off", greenOn ? "ON" : "off"); // ошибка датчика вместо температуры
  }
}

void computeDuty() {                                // расчёт мощности нагрева по ошибке температуры
  if (!heatingOn || !sensorOk) {                    // если нагрев выключен или датчик не отвечает
    dutyPercent = 0.0;                              // мощность ноль (безопасность)
    return;                                         // дальше считать нечего
  }
  float err = setPoint - currentTemp;               // ошибка: сколько градусов не хватает до уставки
  float d = err * PWM_GAIN;                         // мощность пропорциональна ошибке
  if (d < 0.0) d = 0.0;                             // выше уставки нагрев не включаем
  if (d > 100.0) d = 100.0;                         // больше 100 % быть не может
  dutyPercent = d;                                  // запоминаем рассчитанную мощность
}

void updatePwm() {                                  // формирование медленного ШИМ на выходе, вызывается в каждом проходе loop
  unsigned long now = millis();                     // текущее время
  if (now - pwmWindowStart >= PWM_PERIOD_MS) {      // если окно ШИМ закончилось
    pwmWindowStart += PWM_PERIOD_MS;                // начинаем следующее окно
  }
  unsigned long onTime = (unsigned long)(dutyPercent * PWM_PERIOD_MS / 100.0); // сколько мс из окна выход включён
  bool on = (now - pwmWindowStart) < onTime;        // выход включён, пока не прошло onTime от начала окна

  if (on != heatPinState) {                         // если состояние выхода нужно сменить
    heatPinState = on;                              // запоминаем новое состояние
    digitalWrite(HEAT_PIN, on ? HIGH : LOW);        // подаём или снимаем сигнал на SSR
  }

  bool red = (dutyPercent > 0.0);                   // красный горит непрерывно, пока мощность нагрева больше нуля
  if (red != redOn) {                               // если состояние красного нужно сменить
    redOn = red;                                    // запоминаем новое состояние
    digitalWrite(LED_RED, redOn ? HIGH : LOW);      // включаем или гасим красный светодиод
  }
}

void updateLeds() {                                 // управление зелёным светодиодом (температура в норме)
  static bool inBand = false;                       // запоминаем, находимся ли мы сейчас в зоне «температура в норме»
  bool green = false;                               // по умолчанию зелёный погашен

  if (heatingOn && sensorOk) {                      // зелёный работает, только если нагрев включён и датчик исправен
    float diff = currentTemp - setPoint;            // отклонение от уставки: минус = холоднее, плюс = горячее
    if (inBand) {                                   // если уже были в норме
      inBand = fabs(diff) <= TEMP_TOL + TEMP_HYST;  // выходим из нормы только за пределами допуска + гистерезис
    } else {                                        // если не были в норме
      inBand = fabs(diff) <= TEMP_TOL;              // входим в норму, когда отклонение в пределах допуска
    }
    green = inBand;                                 // зелёный горит, пока температура в норме
  } else {                                          // нагрев выключен или датчик не отвечает
    inBand = false;                                 // сбрасываем состояние «в норме»
  }

  if (green != greenOn) {                           // если состояние зелёного изменилось
    greenOn = green;                                // запоминаем новое состояние
    digitalWrite(LED_GREEN, greenOn ? HIGH : LOW);  // включаем или гасим зелёный светодиод
    printStatus();                                  // печатаем состояние при смене зелёного
  }
}

void updateSensor() {                               // неблокирующее чтение температуры, вызывается в каждом проходе loop
  static unsigned long lastRequest = 0;             // время последнего запроса измерения
  static bool pending = false;                      // true, пока идёт измерение (датчик считает до 750 мс)

  if (!pending && millis() - lastRequest >= 1000) { // если не ждём результат и прошла секунда с прошлого запроса
    sensors.requestTemperatures();                  // запускаем новое измерение
    lastRequest = millis();                         // запоминаем время запроса
    pending = true;                                 // отмечаем, что ждём результат
  } else if (pending && millis() - lastRequest >= 800) { // если ждём и прошло 800 мс (измерение готово)
    float t = sensors.getTempCByIndex(0);           // читаем температуру первого датчика на шине
    pending = false;                                // измерение прочитано, больше не ждём
    if (t == DEVICE_DISCONNECTED_C || t == 85.0) {  // -127 = датчик не найден, 85.0 = ошибка питания/запуска датчика
      sensorOk = false;                             // отмечаем, что датчик не отвечает
    } else {                                        // значение корректное
      sensorOk = true;                              // отмечаем, что датчик работает
      currentTemp = t;                              // запоминаем температуру
    }
    computeDuty();                                  // пересчитываем мощность нагрева по новой температуре
    updateLeds();                                   // пересчитываем зелёный светодиод
    printStatus();                                  // выводим состояние в монитор порта раз в секунду
  }
}

void updateEncoder() {                              // чтение вращения энкодера, вызывается в каждом проходе loop
  encState = ((encState << 2) | (digitalRead(ENC_CLK) << 1) | digitalRead(ENC_DT)) & 0x0F; // сдвигаем старое состояние и добавляем новое
  encAccum += encTable[encState];                   // добавляем шаг +1, -1 или 0 по таблице

  if (encAccum >= 4 || encAccum <= -4) {            // накопился полный щелчок (4 шага)
    int dir = (encAccum > 0 ? 1 : -1) * ENC_DIR;    // направление: +1 вправо, -1 влево (с учётом ENC_DIR)
    encAccum = 0;                                   // обнуляем накопитель для следующего щелчка

    int newSet = setPoint + dir * TEMP_STEP;        // вычисляем новую уставку
    if (newSet < TEMP_MIN) newSet = TEMP_MIN;       // не даём уйти ниже минимума
    if (newSet > TEMP_MAX) newSet = TEMP_MAX;       // не даём уйти выше максимума

    if (newSet != setPoint) {                       // если уставка реально изменилась
      setPoint = newSet;                            // запоминаем новую уставку
      computeDuty();                                // сразу пересчитываем мощность под новую уставку
      updateLeds();                                 // сразу пересчитываем зелёный светодиод
      printStatus();                                // выводим состояние в монитор порта
    }
  }
}

void updateButton() {                               // чтение кнопки энкодера с антидребезгом
  static int lastReading = HIGH;                    // последнее прочитанное значение пина (HIGH = не нажата)
  static int stableState = HIGH;                    // устойчивое состояние кнопки после антидребезга
  static unsigned long lastChange = 0;              // время последнего изменения значения пина

  int reading = digitalRead(ENC_SW);                // читаем текущее значение пина кнопки
  if (reading != lastReading) {                     // если значение изменилось
    lastChange = millis();                          // запоминаем время изменения
    lastReading = reading;                          // запоминаем новое значение
  }

  if (millis() - lastChange > 30 && reading != stableState) { // значение держится дольше 30 мс и отличается от устойчивого
    stableState = reading;                          // принимаем новое устойчивое состояние
    if (stableState == LOW) {                       // LOW означает, что кнопку нажали
      heatingOn = !heatingOn;                       // переключаем нагрев: вкл ↔ выкл
      computeDuty();                                // сразу пересчитываем мощность (при выключении станет 0)
      updateLeds();                                 // сразу обновляем зелёный светодиод
      printStatus();                                // выводим состояние в монитор порта
    }
  }
}

void setup() {                                      // выполняется один раз при старте
  Serial.begin(115200);                             // запускаем монитор порта
  pinMode(ENC_CLK, INPUT_PULLUP);                   // CLK как вход с подтяжкой к питанию
  pinMode(ENC_DT, INPUT_PULLUP);                    // DT как вход с подтяжкой к питанию
  pinMode(ENC_SW, INPUT_PULLUP);                    // SW как вход с подтяжкой к питанию
  encState = ((digitalRead(ENC_CLK) << 1) | digitalRead(ENC_DT)) & 0x0F; // запоминаем начальное состояние энкодера

  pinMode(LED_RED, OUTPUT);                         // пин красного светодиода как выход
  pinMode(LED_GREEN, OUTPUT);                       // пин зелёного светодиода как выход
  pinMode(HEAT_PIN, OUTPUT);                        // пин выхода ШИМ для SSR как выход
  digitalWrite(LED_RED, LOW);                       // красный выключен при старте
  digitalWrite(LED_GREEN, LOW);                     // зелёный выключен при старте
  digitalWrite(HEAT_PIN, LOW);                      // выход ШИМ выключен при старте (нагрев всегда стартует выключенным)

  sensors.begin();                                  // запускаем работу с датчиком DS18B20
  sensors.setResolution(12);                        // разрешение 12 бит (шаг 0.0625 °C, измерение до 750 мс)
  sensors.setWaitForConversion(false);              // не блокируем программу на время измерения
  Serial.printf("DS18B20 sensors found: %d\n", sensors.getDeviceCount()); // сколько датчиков нашли на шине (должен быть 1)

  pwmWindowStart = millis();                        // начинаем первое окно ШИМ
  Serial.println("Teppanyaki controller: encoder + sensor + LEDs + PWM"); // заголовок в монитор порта
  printStatus();                                    // печатаем стартовое состояние
}

void loop() {                                       // основной цикл
  updateSensor();                                   // проверяем, не пора ли измерить или прочитать температуру
  updateEncoder();                                  // проверяем вращение энкодера
  updateButton();                                   // проверяем нажатие кнопки
  updatePwm();                                      // формируем ШИМ на выходе и управляем красным светодиодом
}