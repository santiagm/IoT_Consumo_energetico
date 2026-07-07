$env:IDF_PATH="C:\esp\v6.0\esp-idf"
$env:IDF_TOOLS_PATH="C:\Espressif\tools_idf_v6_0_0"

. "C:\esp\v6.0\esp-idf\export.ps1"

idf.py fullclean
idf.py build