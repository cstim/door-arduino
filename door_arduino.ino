#include <RBD_Timer.h> // https://github.com/alextaujenis/RBD_Timer
#include <RBD_Light.h> // https://github.com/alextaujenis/RBD_Light

// The Bounce2 library http://playground.arduino.cc/Code/Bounce
#include <Bounce2.h>
#include <BounceAnalog.h> // a variation of Bounce2 with analog input

/*
  Garage-Door State machine 2

  Christian Stimming, 2016, Hamburg, Germany
 */

// Names for the output pins:
const int pinOutMotorOn = 7;
const int pinOutMotorUp = 6;
const int pinOutWarnLight = 5;
const int pinOutRoomLight = 4;
const int pinOutArduinoLedPin = 13;

const int pinInButton = 3;
const int pinInButtonDown = 2;
const int pinInLightswitch = 0;
const int pinAnalogInOutsideButton = 0;
const int pinAnalogInPhotosensor = 5;

// Active if the serial output should be compiled in.
//#define DEBUG

// /////////////////////////////

/// A management class for a recorded timestamp that should not be older than
/// some constraint
class ConstrainedTimestamp {
    unsigned long m_value;
    const unsigned long m_maxAge;
  public:
    ConstrainedTimestamp(unsigned long maxAge)
        : m_value(0)
        , m_maxAge(maxAge)
    {}
    void setValue(unsigned long value)
    {
        const unsigned long now = millis();
        const unsigned long oldestValue =
                (now > m_maxAge) ? (now - m_maxAge) : 0;
        if (value >= now) {
            m_value = now;
        } else if (value <= oldestValue) {
            m_value = oldestValue;
        } else {
            m_value = value;
        }
    }
    unsigned long getValue() const { return m_value; }
};


// The relays are active-low, so we better define readable names for this
#define RELAY_ON LOW
#define RELAY_OFF HIGH

/** The states that the door can have*/
enum State {
  DOOR_DOWN
  , DOOR_UP
  , DOOR_MOVING_UP
  , DOOR_MOVING_DOWN
  , DOOR_MOVING_DOWN_PAUSED
};
/** This is the door's actual state */
State state = DOOR_DOWN;
const unsigned long c_moveDurationTotal =
#ifdef DEBUG
  5000L;
#else
  17000L; // total movement takes this milliseconds; DEBUG: 2000
#endif
ConstrainedTimestamp g_lastMoveStart(c_moveDurationTotal);
unsigned long g_lastMovePartCompleted = 0;
const unsigned long c_moveTurnaroundPause = 300; // extra waiting time when switching from one direction to the other
const unsigned long c_movingDownPause = 800; // If the lightswitch was blocked, wait for this time
const unsigned long c_waitingTimeBeforeRecloseDaylight =
#ifdef DEBUG
    10*1000L;
#else
    // Don't forget the trailing "L"!!!
    720*1000L; // 12 minutes (720 seconds) before reclose during daylight
#endif
const unsigned long c_waitingTimeBeforeRecloseNight =
#ifdef DEBUG
    3*1000L;
#else
    180*1000L; // 3 minutes (180 sec) before reclose at night
#endif
const unsigned long c_waitingTimeBeforeReallyReclose =
#ifdef DEBUG
  5*1000L;
#else
  10*1000L; // 10 seconds of warning; DEBUG: 3 seconds
#endif

static inline unsigned int currentMoveCompletedToPercent() {
  return 100L * g_lastMovePartCompleted / c_moveDurationTotal;
}

// The wrappers for the input buttons debouncing
Bounce inButtonDebounce;
Bounce inButtonDownDebounce;
BounceAnalog inButtonOutsideDebounce;
const unsigned long c_debounceDelay = 40;
unsigned long g_lastLightswitchBlocked = 0;

// The wrapper for the warning light output
RBD::Light outWarnLightTimer;
RBD::Light outRoomLightTimer;
RBD::Light outArduinoLed;
const unsigned long c_blinkTime = 200; // milliseconds

// The timers for starting a reclosing
RBD::Timer doorUpStartReclose;
RBD::Timer doorUpReallyReclose;
RBD::Timer doorDownPausing;
int g_ambientLightDarkValue = 1023; // The last value for the dark ambient light (=HIGH input)

// The variable to ensure printing only every 8th time some debug output
int g_continuous_printing = 0;

inline void outRoomLightSwitchOn() {
  outRoomLightTimer.blink(150, 150, 1);
}

// the setup routine runs once when you press reset:
void setup() {
  // initialize the digital pin as an output.
  pinMode(pinOutMotorOn, OUTPUT);
  pinMode(pinOutMotorUp, OUTPUT);
  outWarnLightTimer.setupPin(pinOutWarnLight);
  outWarnLightTimer.fade(c_blinkTime, c_blinkTime, c_blinkTime, c_blinkTime, 2);

  outRoomLightTimer.setupPin(pinOutRoomLight, true, true); // digital=true, inverted=true
  outRoomLightSwitchOn();
  outArduinoLed.setupPin(pinOutArduinoLedPin, true); // digital=true
  outArduinoLed.blink(2000, 2000); // In DOOR_DOWN, do some slow blinking to show we are alive

  digitalWrite(pinOutMotorOn, RELAY_OFF);
  digitalWrite(pinOutMotorUp, RELAY_OFF);

  pinMode(pinInButton, INPUT_PULLUP);
  inButtonDebounce.attach(pinInButton);
  inButtonDebounce.interval(c_debounceDelay);

  pinMode(pinInButtonDown, INPUT_PULLUP);
  inButtonDownDebounce.attach(pinInButtonDown);
  inButtonDownDebounce.interval(c_debounceDelay);

  pinMode(pinInLightswitch, INPUT_PULLUP);

  // The input button that has a potentiometer behaviour (due to humidity)
  inButtonOutsideDebounce.attach(pinAnalogInOutsideButton);
  inButtonOutsideDebounce.interval(c_debounceDelay);
  inButtonOutsideDebounce.setCurrentAsMax(); // calibrate the current "high" value of the outside button

#ifdef DEBUG
  Serial.begin(9600);
#endif
  doorUpStartReclose.setTimeout(c_waitingTimeBeforeRecloseDaylight);
  doorUpReallyReclose.setTimeout(c_waitingTimeBeforeReallyReclose);
  doorDownPausing.setTimeout(c_movingDownPause);
  doorUpStartReclose.stop();
  doorUpReallyReclose.stop();
  doorDownPausing.stop();
}

inline void transitionTo_MOVING_DOWN_PAUSED() {
    state = DOOR_MOVING_DOWN_PAUSED;
    digitalWrite(pinOutMotorOn, RELAY_OFF);
    outWarnLightTimer.fade(c_blinkTime, c_blinkTime, c_blinkTime, c_blinkTime);
    doorDownPausing.restart();
}

// the loop routine runs over and over again forever:
void loop() {

  // Update for the input buttons
  const bool onInButtonOutsideChanged = inButtonOutsideDebounce.update();
  const bool onInButtonOutsidePressed = onInButtonOutsideChanged && (inButtonOutsideDebounce.read() == LOW);

  const bool onInButtonChanged = inButtonDebounce.update();
  const bool onInButtonPressed = onInButtonChanged && (inButtonDebounce.read() == LOW);

  const bool onInButtonDownChanged = inButtonDownDebounce.update();
  const bool onInButtonDownPressed = onInButtonDownChanged && (inButtonDownDebounce.read() == LOW);

  const bool onInLightswitchBlocked = (digitalRead(pinInLightswitch) == HIGH);
  if (onInLightswitchBlocked)
  {
      g_lastLightswitchBlocked = millis();
  }

  // Update for the timed output LED
  outWarnLightTimer.update();
  outRoomLightTimer.update();
  outArduinoLed.update();

#ifdef DEBUG
  int val;
  g_continuous_printing++;
  if ((g_continuous_printing & 0xFFF) == 0) {
//    Serial.print("Photosensor = ");
//    val = analogRead(pinAnalogInPhotosensor);
//    Serial.print(val);
    Serial.print(" analogButton = ");
    val = analogRead(pinAnalogInOutsideButton);
    Serial.print(val);
    Serial.print(" inLightswitch = ");
    Serial.println(onInLightswitchBlocked);
  }

  if (onInButtonOutsideChanged) {
    Serial.print("onInButtonOutsideChanged  -- ");
  }
  if (onInButtonChanged) {
    Serial.print("onInButtonChanged, now = ");
    Serial.print(inButtonDebounce.read());
    Serial.print(" at ");
    Serial.println(millis());
  }
  if (onInButtonDownChanged) {
    Serial.print("onInButtonDownChanged, now = ");
    Serial.print(inButtonDownDebounce.read());
    Serial.print(" at ");
    Serial.println(millis());
  }
#endif

  switch (state) {
    case DOOR_DOWN:
      // Door down/closed: Make sure motor is off
      digitalWrite(pinOutMotorOn, RELAY_OFF);
      digitalWrite(pinOutMotorUp, RELAY_OFF);
      // State change: Only when the Up-Button is pressed
      if (onInButtonPressed || onInButtonOutsidePressed) {
        state = DOOR_MOVING_UP;
        g_lastMoveStart.setValue(millis());
        // Store the current reading of the ambient light photo sensor. Door is closed = ambient light is dark = input pin is HIGH
        g_ambientLightDarkValue = constrain(analogRead(pinAnalogInPhotosensor), 128, 1023);
#ifdef DEBUG
        Serial.print(millis());
        Serial.print(": state DOWN -> MOVING_UP; Resetting ambientLightDarkValue to ");
        Serial.println(g_ambientLightDarkValue);
#endif
        outWarnLightTimer.on();
        outRoomLightSwitchOn();
      }
      break;

    case DOOR_UP:
      // Door up/open: Make sure motor is off
      digitalWrite(pinOutMotorOn, RELAY_OFF);
      digitalWrite(pinOutMotorUp, RELAY_OFF);
      // Special rule on Up-Button, if the reclose blinking is ongoing: Cancel the reclose
      if (doorUpReallyReclose.isActive() && (onInButtonPressed || onInLightswitchBlocked)) {
        doorUpReallyReclose.stop();
        doorUpStartReclose.restart();
        outWarnLightTimer.off();
      } else
      // State change can be because of multiple things
      if (onInButtonPressed || onInButtonOutsidePressed || onInButtonDownPressed || doorUpReallyReclose.onExpired()) {
        g_lastMoveStart.setValue(millis());
        doorUpStartReclose.stop();
        doorUpReallyReclose.stop();
        g_lastMovePartCompleted = 0;
        outRoomLightSwitchOn();
        if (onInButtonDownPressed) {
            inButtonOutsideDebounce.setCurrentAsMax(); // calibrate the current "high" value of the outside button
        }
        if (onInLightswitchBlocked || millis() <= g_lastLightswitchBlocked + c_movingDownPause) {
          // Oh, we had somebody blocking the lightswitch, so go into PAUSED mode immediately
          transitionTo_MOVING_DOWN_PAUSED();
#ifdef DEBUG
          Serial.print(millis());
          Serial.print(": state DOOR_UP -> MOVING_DOWN_PAUSED due to lightswitchBlocked. lastMoveStart=");
          Serial.println(g_lastMoveStart.getValue());
#endif
          //delay(1); // maybe this fixes a potential wrong state transition
        } else {
          // Any button was pressed or timer expired => State change: Now move downwards
          state = DOOR_MOVING_DOWN;
          outWarnLightTimer.on();
          doorDownPausing.stop();
#ifdef DEBUG
          Serial.print(millis());
          Serial.print(": state DOOR_UP -> MOVING_DOWN; lastMoveStart=");
          Serial.println(g_lastMoveStart.getValue());
#endif
        }
      }
      // While the door is open, check for the timer timeout of re-closing
      if (doorUpStartReclose.onExpired()) {
        doorUpReallyReclose.restart();
        outWarnLightTimer.blink(c_blinkTime, c_blinkTime);
        outRoomLightSwitchOn();
      }
      break;

    case DOOR_MOVING_DOWN:
      // Door moving downwards/closing
      digitalWrite(pinOutMotorOn, RELAY_ON);
      digitalWrite(pinOutMotorUp, RELAY_OFF);

      // State change: Have we reached the DOOR_DOWN position?
      if (millis() - g_lastMoveStart.getValue() > c_moveDurationTotal) {
        state = DOOR_DOWN;
        outWarnLightTimer.off();
        outArduinoLed.blink(2000, 2000); // In DOOR_DOWN, do some slow blinking to show we are alive
#ifdef DEBUG
        Serial.print(millis());
        Serial.print(": state MOVING_DOWN -> DOOR_DOWN; lastMoveStart=");
        Serial.print(g_lastMoveStart.getValue());
        Serial.print(" diff=");
        Serial.print(millis() - g_lastMoveStart.getValue());
        Serial.print(" moveTotal=");
        Serial.println(c_moveDurationTotal);
#endif
      }

      // State change: Pressed button for changing direction?
      if (onInButtonPressed || onInButtonOutsidePressed) {
        // Some up-button was pressed => State change: Now move up again
        state = DOOR_MOVING_UP;
        digitalWrite(pinOutMotorOn, RELAY_OFF);
        g_lastMovePartCompleted = millis() - g_lastMoveStart.getValue();
        const unsigned long moveRemaining = c_moveDurationTotal - g_lastMovePartCompleted;
        delay(c_moveTurnaroundPause);
        g_lastMoveStart.setValue(millis() - moveRemaining);
#ifdef DEBUG
        Serial.print("state MOVING_DOWN -> MOVING_UP. lastMovePartCompleted[%] = ");
        Serial.println(currentMoveCompletedToPercent());
#endif
      } else if (onInLightswitchBlocked) {
        // Light switch is blocked => State change: Go into PAUSED state
        transitionTo_MOVING_DOWN_PAUSED();
        g_lastMovePartCompleted = millis() - g_lastMoveStart.getValue();
#ifdef DEBUG
        Serial.print(millis());
        Serial.print(": state MOVING_DOWN -> PAUSED due to lightswitchBlocked. lastMovePartCompleted[%] = ");
        Serial.println(currentMoveCompletedToPercent());
#endif
        delay(1); // otherwise we get a wrong state transition
      }
      break;

    case DOOR_MOVING_DOWN_PAUSED:
      digitalWrite(pinOutMotorOn, RELAY_OFF);
      digitalWrite(pinOutMotorUp, RELAY_OFF);

      // State change: Pressed button for changing direction?
      if (onInButtonPressed || onInButtonOutsidePressed) {
        // Some button was pressed => State change: Now move up again
        state = DOOR_MOVING_UP;
        outWarnLightTimer.on();
        const unsigned long moveRemaining = c_moveDurationTotal - g_lastMovePartCompleted;
        g_lastMoveStart.setValue(millis() - moveRemaining);
#ifdef DEBUG
        Serial.print("state PAUSED -> MOVING_UP; lastMovePartCompleted[%] = ");
        Serial.print(currentMoveCompletedToPercent());
        Serial.print(" lastMoveStart=");
        Serial.println(g_lastMoveStart.getValue());
#endif
        delay(1); // otherwise we get a wrong state transition
      } else if (onInLightswitchBlocked) {
        // Light switch is still blocked: Still waiting for pause time
        doorDownPausing.restart();
      } else if (doorDownPausing.onExpired()) {
        // Light switch was free again for long enough => State change: Continue moving downwards
        state = DOOR_MOVING_DOWN;
        outWarnLightTimer.on();
        g_lastMoveStart.setValue(millis() - g_lastMovePartCompleted);
        digitalWrite(pinOutMotorOn, RELAY_ON);
#ifdef DEBUG
        Serial.print(millis());
        Serial.print(": state PAUSED -> MOVING_DOWN; lastMovePartCompleted[%] = ");
        Serial.print(currentMoveCompletedToPercent());
        Serial.print("; lastMoveStart=");
        Serial.println(g_lastMoveStart.getValue());
#endif
        delay(1); // otherwise we get a wrong state transition
      }
      break;

    case DOOR_MOVING_UP:
      // Door moving upwards/opening
      digitalWrite(pinOutMotorOn, RELAY_ON);
      digitalWrite(pinOutMotorUp, RELAY_ON);

      // State change: Have we reached the DOOR_UP position?
      if (millis() - g_lastMoveStart.getValue() > c_moveDurationTotal) {
        state = DOOR_UP;
        outWarnLightTimer.off();
        inButtonOutsideDebounce.setCurrentAsMax(); // calibrate the current "high" value of the outside button

        // What is the current ambient light? Is it brighter (=input pin is LOW), 
        // say than 50% of the "dark" threshold? Then we have daylight.
        const int photoValue = analogRead(pinAnalogInPhotosensor);
        const bool daylight = true; // DISABLED FOR NOW! //(photoValue * 2 < ambientLightDarkValue) || (photoValue > 1023);
        const long waitingTime =  daylight
                                      ? c_waitingTimeBeforeRecloseDaylight
                                      : c_waitingTimeBeforeRecloseNight;
        doorUpStartReclose.setTimeout(waitingTime);
#ifdef DEBUG
        Serial.print("state MOVING_UP -> DOOR_UP; Set restarting timer with daylight=");
        Serial.print(daylight);
        Serial.print(" to timeout=");
        Serial.println(waitingTime);
#endif
        doorUpStartReclose.restart();
        outArduinoLed.blink(400, daylight ? 400 : 800); // in DOOR_UP state we blink somewhat faster, also depending on daylight
      }

      // State change: Pressed button for changing direction?
      if (onInButtonDownPressed) {
        const unsigned long movePartCompleted = millis() - g_lastMoveStart.getValue();
        transitionTo_MOVING_DOWN_PAUSED();
        g_lastMovePartCompleted = c_moveDurationTotal - movePartCompleted;
#ifdef DEBUG
        Serial.print(millis());
        Serial.println(": state MOVING_UP -> MOVING_DOWN_PAUSED; lastMovePartCompleted[%] = ");
        Serial.println(currentMoveCompletedToPercent());
#endif
      }
      break;
  }
}

