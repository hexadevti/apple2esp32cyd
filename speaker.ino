void speakerSetup() {
    digitalWrite(SPEAKER_PIN, LOW);
    pinMode(SPEAKER_PIN, OUTPUT);
    
}

void speakerToggle() {
  speaker_state = !speaker_state;
  
    digitalWrite(SPEAKER_PIN, speaker_state ? HIGH : LOW);
    
}