// ADB executable entry (self-tests, automated routes); the APK uses jni_main.cpp.
int me_runtime_main(int argc, char** argv);
int main(int argc, char** argv) { return me_runtime_main(argc, argv); }
