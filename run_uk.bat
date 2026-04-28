@echo off  
set VK_ICD_FILENAMES=S:\bld\vboxgpu\tests\UltraKill\vbox_icd.json  
set VK_LOADER_LAYERS_DISABLE=*  
cd /d S:\bld\vboxgpu\tests\UltraKill  
ULTRAKILL.exe -screen-width 800 -screen-height 600 -screen-fullscreen 0  
