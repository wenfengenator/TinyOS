import taudio

print("audio ready:", taudio.ready())
print("diagnostics:", taudio.info())
for frequency in (262, 330, 392, 523):
    taudio.tone(frequency, 140, 70)