Set-Location "C:\Users\awa\Documents\trae_projects\a\awa"
$env:IDF_COMPONENT_MANAGER="0"
& "C:\Espressif\frameworks\esp-idf-v5.5.1\tools\legacy_exports\export_legacy.ps1"
idf.py build