/* Macros CMake passes on the compiler command line, for cppcheck. A string define cannot be passed
   with -D the same way from Windows PowerShell 5.1 and PowerShell 7 (quoting differs), so it lives
   here and ci.ps1 hands this file to cppcheck with --include. */
#define ABT_VERSION_STR "0.0.0"
