#!/usr/bin/env bash
set -e

python3 << 'EOF'
path = "src/screens/screen_manager.cpp"
with open(path) as f:
    content = f.read()

# --- formatHHMM: String -> void(char*, size_t) ---
old_def = '''static String formatHHMM(uint32_t unixTime) {
  if (unixTime == 0) return String("--:--");
  time_t t = (time_t)unixTime;
  struct tm* timeInfo = localtime(&t);
  char buf[16];
  strftime(buf, sizeof(buf), "%I:%M %p", timeInfo);
  return String(buf);
}'''
assert content.count(old_def) == 1, f"formatHHMM def: expected 1, found {content.count(old_def)}"
new_def = '''static void formatHHMM(uint32_t unixTime, char* out, size_t outLen) {
  if (unixTime == 0) { snprintf(out, outLen, "--:--"); return; }
  time_t t = (time_t)unixTime;
  struct tm* timeInfo = localtime(&t);
  strftime(out, outLen, "%I:%M %p", timeInfo);
}'''
content = content.replace(old_def, new_def)

# --- formatPassTime: String -> void(char*, size_t) ---
old_def2 = '''static String formatPassTime(uint32_t unixTime) {
  if (unixTime == 0) return "--";
  time_t t = (time_t)unixTime;
  struct tm* ti = localtime(&t);
  int h12 = ti->tm_hour % 12;
  if (h12 == 0) h12 = 12;
  char buf[16];
  snprintf(buf, sizeof(buf), "%d:%02d%s", h12, ti->tm_min, ti->tm_hour < 12 ? "A" : "P");
  return String(buf);
}'''
assert content.count(old_def2) == 1, f"formatPassTime def: expected 1, found {content.count(old_def2)}"
new_def2 = '''static void formatPassTime(uint32_t unixTime, char* out, size_t outLen) {
  if (unixTime == 0) { snprintf(out, outLen, "--"); return; }
  time_t t = (time_t)unixTime;
  struct tm* ti = localtime(&t);
  int h12 = ti->tm_hour % 12;
  if (h12 == 0) h12 = 12;
  snprintf(out, outLen, "%d:%02d%s", h12, ti->tm_min, ti->tm_hour < 12 ? "A" : "P");
}'''
content = content.replace(old_def2, new_def2)

# --- formatPassDate: String -> void(char*, size_t) ---
old_def3 = '''static String formatPassDate(uint32_t unixTime) {
  if (unixTime == 0) return "--";
  time_t t = (time_t)unixTime;
  struct tm* ti = localtime(&t);
  char buf[16];
  strftime(buf, sizeof(buf), "%b%d", ti);
  return String(buf);
}'''
assert content.count(old_def3) == 1, f"formatPassDate def: expected 1, found {content.count(old_def3)}"
new_def3 = '''static void formatPassDate(uint32_t unixTime, char* out, size_t outLen) {
  if (unixTime == 0) { snprintf(out, outLen, "--"); return; }
  time_t t = (time_t)unixTime;
  struct tm* ti = localtime(&t);
  strftime(out, outLen, "%b%d", ti);
}'''
content = content.replace(old_def3, new_def3)

with open(path, "w") as f:
    f.write(content)
print("Rewrote formatHHMM/formatPassTime/formatPassDate to write into caller buffers")
EOF
