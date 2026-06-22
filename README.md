# MatrixScreenSaver
A fully configurable Matrix ScreenSaver

choose color or multicolor in multy line or multiletter
choose amount of lines
choose the amount of brihgts efects
choose the letter size
choose falling speed
choose changing letters speed
choose the amount of changing letters in a chain
choose minimiun size of the string
choose the maximun size of the string
choose multi speed strings
choose eliminating free spaces
choose the caracters that will fall down

> Get-FileHash -Path ".\Matrixmio.scr" -Algorithm SHA256

Algorithm       Hash                                                                   Path
---------       ----                                                                   ----
SHA256          44722ECA8601B8ADADE20FCDBF10FE99882DCDF50A8B52F48C1C02CBD2165053       C:\Users\Matrixmio.scr

> Get-FileHash -Path ".\MatrixmioEN.scr" -Algorithm SHA256

Algorithm       Hash                                                                   Path
---------       ----                                                                   ----
SHA256          E1F6DF3AE61F24B13D5B0156A63032683B689890362AC8E50D02567D37D15464       C:\Users\MatrixmioEN.scr


--------------------------
-       NEW UPDATE       -
--------------------------

Add Multilingual Suport
English, French, Spanish, Rusian, deutch and japanese

Add 3D efect (3 deep environment close, media and far away to simulate the effect)
Add Random speed of chains
Add minimum speed of random
Add maximum speed of random
Add minimum measure of chains
Add maximum measure of chains

> Get-FileHash -Path ".\Matrix3D.scr" -Algorithm SHA256

Algorithm       Hash                                                                   Path
---------       ----                                                                   ----
SHA256          BD5C6D6BB2C55916A24DEF034973F0F83001E406E1402AF4ECA75B6C12C65905

Compiling line

cl /EHsc /DUNICODE /D_UNICODE Matrix3D.cpp /link /OUT:Matrix3D.scr /SUBSYSTEM:WINDOWS /DEFAULTLIB:User32.lib /DEFAULTLIB:Gdi32.lib /DEFAULTLIB:Advapi32.lib /DEFAULTLIB:Comctl32.lib /DEFAULTLIB:Comdlg32.lib
