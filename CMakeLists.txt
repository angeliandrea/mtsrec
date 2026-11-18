# Multicam Timecode Sync Recorder, forma corta mtsrec (sezione 9.81); fino alla 0.9 beta 5 WinISORecorder.
# Il nucleo in C++: acquisizione DeckLink, codifica DNxHD con ffmpeg, scrittura dei file.
# Si compila con compila.bat, che prende Visual Studio Build Tools e le librerie da vcpkg.
cmake_minimum_required(VERSION 3.25)
# Le DLL di ffmpeg le copia questo file, non vcpkg: hanno gli stessi nomi e si sovrascriverebbero.
set(VCPKG_APPLOCAL_DEPS OFF)
# Le dipendenze compilate o scaricate a parte (ffmpeg, Python embeddable, SDK WebView2) stanno in una
# cartella sola: MTSREC_DEPS se c'è (la compilazione su GitHub), altrimenti quella di questa macchina.
if(DEFINED ENV{MTSREC_DEPS})
    file(TO_CMAKE_PATH "$ENV{MTSREC_DEPS}" MTSREC_DEPS_BASE)
else()
    file(TO_CMAKE_PATH "$ENV{USERPROFILE}/WinISORecorder-deps" MTSREC_DEPS_BASE)
endif()
set(MTSREC_DEPS "${MTSREC_DEPS_BASE}" CACHE PATH "Cartella delle dipendenze: ffmpeg-statico, python, webview2-sdk")
# Con ffmpeg già compilato (BtbN, più sotto) vcpkg non deve installare niente: a ogni riconfigurazione
# scaricherebbe i suoi strumenti da internet. Resta il ripiego se quella cartella manca.
if(EXISTS "${MTSREC_DEPS}/ffmpeg-n9.0-latest-win64-lgpl-shared-9.0/include/libavcodec/avcodec.h"
   OR EXISTS "${MTSREC_DEPS}/ffmpeg-statico/lib/avcodec.lib"
   OR DEFINED WINISO_FFMPEG_STATICO)
    set(VCPKG_MANIFEST_INSTALL OFF CACHE BOOL "" FORCE)
endif()
project(mtsrec LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
# La versione in un posto solo, VERSIONE.txt, che legge anche l'installatore (sezione 9.73).
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSIONE.txt" WINISO_VERSIONE LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSIONE.txt")
# In numeri per la scheda «Dettagli» del file e per l'MSI: «0.9 beta 6» diventa 0.9.6, «1.0» 1.0.0.
if(NOT WINISO_VERSIONE MATCHES "^([0-9]+)\\.([0-9]+)( beta ([0-9]+))?$")
    message(FATAL_ERROR "VERSIONE.txt: «${WINISO_VERSIONE}» non è nella forma «0.9 beta 6» o «1.0»")
endif()
set(WINISO_V1 ${CMAKE_MATCH_1})
set(WINISO_V2 ${CMAKE_MATCH_2})
set(WINISO_V3 0)
if(CMAKE_MATCH_4)
    set(WINISO_V3 ${CMAKE_MATCH_4})
endif()
set(WINISO_VERSIONE_NUMERI "${WINISO_V1}.${WINISO_V2}.${WINISO_V3}")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/versione.h.in" "${CMAKE_CURRENT_BINARY_DIR}/versione.h" @ONLY)
# Dove i beta tester mandano il rapporto di prova (Andrea, 17 novembre 2026): è già pubblico sulla pagina.
set(WINISO_EMAIL_RAPPORTI "info@angeliandrea.it")
# Dalla 0.9 beta 8 il rapporto si salva sul Desktop e lo manda il tester come vuole: per mail o dalla pagina
# Filemail di Andrea, che si apre solo premendo il tasto (Andrea, 18 novembre 2026).
set(WINISO_PORTALE_RAPPORTI "https://andrea-angeli.filemail.com/")
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Libreria di Visual C++ dentro il programma: su un'altra macchina non serve installare niente.
# Le DLL di ffmpeg sono compilate con gcc e non la usano.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_options(/utf-8 /W4 /permissive- /Zc:__cplusplus)
# Senza WIN32_LEAN_AND_MEAN: le interfacce DeckLink hanno bisogno della parte COM di windows.h.
add_compile_definitions(UNICODE _UNICODE NOMINMAX
    _CRT_SECURE_NO_WARNINGS _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING)

# Dell'SDK DeckLink si usano solo le interfacce, compilate da midl: il codice degli esempi Blackmagic
# non entra nel programma. Nel repository pubblico i file .idl stanno in third_party\decklink-sdk\include,
# che la licenza dell'SDK permette di ridistribuire; qui l'SDK intero sta accanto al progetto e non si pubblica.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/decklink-sdk/include/DeckLinkAPI.idl")
    set(DECKLINK_IDL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/decklink-sdk/include")
else()
    set(DECKLINK_IDL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sdk/Blackmagic DeckLink SDK 16.0/Win/include")
endif()
set(DECKLINK_GEN "${CMAKE_CURRENT_BINARY_DIR}/decklink")
file(MAKE_DIRECTORY "${DECKLINK_GEN}")
add_custom_command(
    OUTPUT "${DECKLINK_GEN}/DeckLinkAPI_h.h" "${DECKLINK_GEN}/DeckLinkAPI_i.c"
    COMMAND midl /nologo /env x64 /I "${DECKLINK_IDL_DIR}"
            /h DeckLinkAPI_h.h /iid DeckLinkAPI_i.c /tlb DeckLinkAPI.tlb
            "${DECKLINK_IDL_DIR}/DeckLinkAPI.idl"
    WORKING_DIRECTORY "${DECKLINK_GEN}"
    DEPENDS "${DECKLINK_IDL_DIR}/DeckLinkAPI.idl"
    COMMENT "midl: interfacce DeckLink")

# ffmpeg: di base la build LGPL di BtbN, compilata con gcc. Quella di vcpkg è compilata con MSVC,
# che non accetta l'assembler in linea, e codifica il DNxHD 3,5 volte più piano (sezione 9.54).
# vcpkg resta come ripiego se la cartella non c'è.
#
# Dalla sezione 9.81 la strada di base è ffmpeg compilato da noi con ffmpeg\compila-ffmpeg.bat, in
# librerie statiche DENTRO mtsrec.exe: Smart App Control di Windows 11 blocca le DLL non firmate
# e SignPath firma solo quello che nasce dal nostro sorgente. Le altre due restano come ripiego.
set(WINISO_FFMPEG_STATICO "${MTSREC_DEPS}/ffmpeg-statico"
    CACHE PATH "ffmpeg statico da ffmpeg/compila-ffmpeg.bat: include e lib")
set(WINISO_FFMPEG_DIR "${MTSREC_DEPS}/ffmpeg-n9.0-latest-win64-lgpl-shared-9.0"
    CACHE PATH "ffmpeg già compilato: include, lib e bin")
if(EXISTS "${WINISO_FFMPEG_STATICO}/lib/avcodec.lib")
    message(STATUS "ffmpeg statico da ${WINISO_FFMPEG_STATICO}")
    set(FFMPEG_INCLUDE_DIRS "${WINISO_FFMPEG_STATICO}/include")
    set(FFMPEG_LIBRARY_DIRS "${WINISO_FFMPEG_STATICO}/lib")
    # le librerie di sistema le dice ffmpeg stesso, nei suoi .pc
    set(FFMPEG_LIBRARIES avformat avcodec avutil bcrypt ole32 user32)
    set(FFMPEG_DLL "")
    set(WINISO_FFMPEG_LICENZA "${WINISO_FFMPEG_STATICO}/../ffmpeg-9.0.2/COPYING.LGPLv2.1")
elseif(EXISTS "${WINISO_FFMPEG_DIR}/include/libavcodec/avcodec.h")
    message(STATUS "ffmpeg da ${WINISO_FFMPEG_DIR}")
    set(FFMPEG_INCLUDE_DIRS "${WINISO_FFMPEG_DIR}/include")
    set(FFMPEG_LIBRARY_DIRS "${WINISO_FFMPEG_DIR}/lib")
    set(FFMPEG_LIBRARIES avformat avcodec avutil)
    file(GLOB FFMPEG_DLL "${WINISO_FFMPEG_DIR}/bin/*.dll")
else()
    message(STATUS "ffmpeg da vcpkg: più lento, sezione 9.54")
    find_package(FFMPEG REQUIRED)
    file(GLOB FFMPEG_DLL "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/bin/av*.dll"
                         "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/bin/sw*.dll")
endif()

add_executable(mtsrec
    src/main.cpp
    src/codificatore.cpp
    src/formato.cpp
    src/traccia.cpp
    src/ingresso.cpp
    src/esterni.cpp
    src/controlli.cpp
    src/marker.cpp
    src/miniatura.cpp
    src/pagina.cpp
    src/ripara.cpp
    src/sonda.cpp
    src/macchina.cpp
    src/registro.cpp
    src/impostazioni.cpp
    src/carico.cpp
    src/registrazione.cpp
    src/interfaccia.cpp
    src/finestra.cpp
    src/mtsrec.rc
    "${DECKLINK_GEN}/DeckLinkAPI_i.c")
target_include_directories(mtsrec PRIVATE "${DECKLINK_GEN}" "${CMAKE_CURRENT_BINARY_DIR}")   # versione.h per mtsrec.rc
# WebView2 per la finestra del registratore (sezione 9.61): solo intestazioni e il caricatore statico
# dal pacchetto NuGet di Microsoft. Il motore è quello che Windows ha già, niente DLL in più.
set(WINISO_WEBVIEW2_DIR "${MTSREC_DEPS}/webview2-sdk/1.0.2903.40/build/native"
    CACHE PATH "SDK WebView2: include e x64")
target_include_directories(mtsrec SYSTEM PRIVATE "${WINISO_WEBVIEW2_DIR}/include")
target_include_directories(mtsrec SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})   # i loro avvisi non sono nostri
target_link_directories(mtsrec PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(mtsrec PRIVATE ${FFMPEG_LIBRARIES} ole32 oleaut32 ws2_32 powrprof advapi32
    setupapi cfgmgr32 dxgi iphlpapi pdh shell32 "${WINISO_WEBVIEW2_DIR}/x64/WebView2LoaderStatic.lib" version shlwapi user32)
if(FFMPEG_DLL)
    add_custom_command(TARGET mtsrec POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_DLL} "$<TARGET_FILE_DIR:mtsrec>")
endif()

# Il pacchetto: la cartella di compilazione si porta su un'altra macchina così com'è, senza installare
# niente. Dentro Python embeddable in python\ (python.org, mai installato nel sistema)
# e gli script che il registratore e chi lo usa lanciano, in script\.
set(WINISO_PYTHON_DIR "${MTSREC_DEPS}/python-3.13.17-embed-amd64"
    CACHE PATH "Python embeddable da mettere nel pacchetto")
set(WINISO_SCRIPT
    registro_atem.py mappa_porte.py nomi_atem.py ritardi_porte.py progetto_da_registratore.py crea_progetto_resolve.py
    genera_fcpxml.py genera_premiere.py tagli_da_atem.py clip_hyperdeck.py rapporto_prova.py cadenza.py stato_atem.py)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/script/registro_atem.py")
    set(WINISO_SCRIPT_SORGENTE "${CMAKE_CURRENT_SOURCE_DIR}/script")       # repository pubblico
else()
    set(WINISO_SCRIPT_SORGENTE "${CMAKE_CURRENT_SOURCE_DIR}/../script")    # questo progetto
endif()
list(TRANSFORM WINISO_SCRIPT PREPEND "${WINISO_SCRIPT_SORGENTE}/")
# ffprobe.exe non si copia più (sezione 9.81): i MOV li legge mtsrec --sonda.
if(EXISTS "${WINISO_PYTHON_DIR}/python.exe")
    add_custom_command(TARGET mtsrec POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${WINISO_PYTHON_DIR}" "$<TARGET_FILE_DIR:mtsrec>/python")
else()
    message(WARNING "Python embeddable non trovato in ${WINISO_PYTHON_DIR}: il pacchetto userà py -3.13")
endif()
add_custom_command(TARGET mtsrec POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:mtsrec>/script"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${WINISO_SCRIPT} "$<TARGET_FILE_DIR:mtsrec>/script")
# Le due pagine della sezione 10, controllo della macchina e registrazione, in interfaccia\.
# Il registratore le legge da lì a ogni richiesta: si possono ritoccare senza ricompilare.
add_custom_command(TARGET mtsrec POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/interfaccia" "$<TARGET_FILE_DIR:mtsrec>/interfaccia")
# Gli script Python e le pagine fuori dal pacchetto: il ripiego mentre si compila su questa macchina.
# Le licenze di quello che viaggia col programma: ffmpeg LGPL, Python, WebView2, più le note di terzi.
add_custom_command(TARGET mtsrec POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:mtsrec>/licenses"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/licenze/THIRD-PARTY NOTICES.txt"
            "${CMAKE_CURRENT_SOURCE_DIR}/licenze/LICENSE.txt" "$<TARGET_FILE_DIR:mtsrec>/licenses"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/licenze/BETA TESTER GUIDE.txt"
            "$<TARGET_FILE_DIR:mtsrec>"
    # Il manuale d'uso, che il tasto Help apre (sezione 9.77): si rigenera con manuale\crea-manuale.py
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/manuale/Multicam Timecode Sync Recorder User Manual.pdf"
            "$<TARGET_FILE_DIR:mtsrec>")
if(WINISO_FFMPEG_LICENZA AND EXISTS "${WINISO_FFMPEG_LICENZA}")
    add_custom_command(TARGET mtsrec POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${WINISO_FFMPEG_LICENZA}" "$<TARGET_FILE_DIR:mtsrec>/licenses/FFmpeg LICENSE.txt")
elseif(EXISTS "${WINISO_FFMPEG_DIR}/LICENSE.txt")
    add_custom_command(TARGET mtsrec POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${WINISO_FFMPEG_DIR}/LICENSE.txt" "$<TARGET_FILE_DIR:mtsrec>/licenses/FFmpeg LICENSE.txt")
endif()
if(EXISTS "${WINISO_WEBVIEW2_DIR}/../../LICENSE.txt")
    add_custom_command(TARGET mtsrec POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${WINISO_WEBVIEW2_DIR}/../../LICENSE.txt" "$<TARGET_FILE_DIR:mtsrec>/licenses/WebView2 LICENSE.txt")
endif()
target_compile_definitions(mtsrec PRIVATE WINISO_VERSIONE="${WINISO_VERSIONE}" WINISO_EMAIL_RAPPORTI="${WINISO_EMAIL_RAPPORTI}"
    WINISO_PORTALE_RAPPORTI="${WINISO_PORTALE_RAPPORTI}")
target_compile_definitions(mtsrec PRIVATE WINISO_SCRIPT_DIR="${WINISO_SCRIPT_SORGENTE}"
    WINISO_PAGINE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/interfaccia")
