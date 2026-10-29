// Text the recomp adds in French, German, Italian, Spanish and Indonesian: Double Agent recomp's wording where the
// English is the same, in sentence case with German's nouns capitalized; the panel's rows and the game's menu show it
// in capitals. Indonesian has no game strings on any disc, so only this text changes with it.
#include <algorithm>
#include <cctype>
#include <iterator>
#include <map>
#include <mutex>

#include "common/options.h"
#include "common/translations.h"

namespace {

// in game::kLanguages order after int; an empty one (the touch rows the other languages lack) stays English
struct Entry {
    std::string_view english;
    std::u16string_view text[5];
};
constexpr Entry kEntries[] = {
    // the settings pages and their sections
    {"GENERAL", {u"Général", u"Allgemein", u"Generale", u"General", u"Umum"}},
    {"DISPLAY", {u"Affichage", u"Anzeige", u"Video", u"Pantalla", u"Tampilan"}},
    {"GRAPHICS", {u"Graphismes", u"Grafik", u"Grafica", u"Gráficos", u"Grafis"}},
    {"PRESS A KEY", {u"Appuyez sur une touche", u"Taste drücken", u"Premi un tasto", u"Pulsa una tecla", u"Tekan tombol"}},
    {"UNBOUND", {u"Non assigné", u"Nicht belegt", u"Non assegnato", u"Sin asignar", u"Belum diatur"}},
    {"TOUCH", {{}, {}, {}, {}, u"Sentuh"}},
    {"DRAG TO MOVE, PINCH OR - + TO RESIZE", {{}, {}, {}, {}, u"Seret untuk memindah, cubit atau - + untuk mengubah ukuran"}},
    {"QUALITY", {u"Qualité", u"Qualität", u"Qualità", u"Calidad", u"Kualitas"}},
    {"PERFORMANCE", {u"Performances", u"Leistung", u"Prestazioni", u"Rendimiento", u"Performa"}},
    {"TOUCH CONTROLS", {u"Commandes tactiles", u"Touch-Steuerung", u"Comandi touch", u"Controles táctiles", u"Kontrol sentuh"}},
    {"UPDATES", {u"Mises à jour", u"Updates", u"Aggiornamenti", u"Actualizaciones", u"Pembaruan"}},
    {"CONTROLS", {{}, {}, {}, {}, u"Kontrol"}},

    // the settings rows
    {"RESOLUTION", {u"Résolution", u"Auflösung", u"Risoluzione", u"Resolución", u"Resolusi"}},
    {"RENDER RESOLUTION", {u"Résolution de rendu", u"Render-Auflösung", u"Risoluzione di rendering", u"Resolución de renderizado", u"Resolusi render"}},
    {"VERTICAL SYNC", {u"Synchro verticale", u"V-Sync", u"Sincronia verticale", u"Sincronismo vertical", u"Sinkronisasi vertikal"}},
    {"MAX FRAME RATE", {u"Images par seconde max.", u"Max. Bildrate", u"Frame rate massimo", u"Fotogramas por segundo máx.", u"Frame rate maks."}},
    {"FOG", {u"Brouillard", u"Nebel", u"Nebbia", u"Niebla", u"Kabut"}},
    {"CAR REFLECTIONS", {u"Reflets des voitures", u"Fahrzeugreflexionen", u"Riflessi delle auto", u"Reflejos de los coches", u"Pantulan mobil"}},
    {"FULL", {u"Complet", u"Voll", u"Pieno", u"Completo", u"Penuh"}},
    {"HALF", {u"Moitié", u"Halb", u"Metà", u"Mitad", u"Setengah"}},
    {"QUARTER", {u"Quart", u"Viertel", u"Un quarto", u"Un cuarto", u"Seperempat"}},
    {"ROAD REFLECTIONS",{u"Reflets de la route", u"Straßenreflexionen", u"Riflessi della strada", u"Reflejos de la carretera", u"Pantulan jalan"}},
    {"SHADOWS",{u"Ombres", u"Schatten", u"Ombre", u"Sombras", u"Bayangan"}},
    {"PARTICLE EFFECTS", {u"Effets de particules", u"Partikeleffekte", u"Effetti particellari", u"Efectos de partículas", u"Efek partikel"}},
    {"FALLING RAIN", {u"Pluie", u"Regen", u"Pioggia", u"Lluvia", u"Hujan turun"}},
    {"SCREEN RAINDROPS", {u"Gouttes de pluie à l'écran", u"Regentropfen auf dem Bildschirm", u"Gocce di pioggia sullo schermo", u"Gotas de lluvia en pantalla", u"Tetes hujan di layar"}},
    {"SHADER PRELOADING", {u"Préchargement des shaders", u"Shader vorladen", u"Precaricamento shader", u"Precarga de shaders", u"Muat shader di awal"}},
    {"MOTION BLUR", {u"Flou de mouvement", u"Bewegungsunschärfe", u"Sfocatura di movimento", u"Desenfoque de movimiento", u"Blur gerakan"}},
    {"YELLOW TINT", {u"Teinte jaune", u"Gelbstich", u"Tinta gialla", u"Tinte amarillo", u"Filter kuning"}},
    {"BLOOM", {u"Flou lumineux", u"Bloom", u"Bloom", u"Resplandor", u"Bloom"}},
    {"ASPECT RATIO", {{}, {}, {}, {}, u"Rasio aspek"}},
    {"FULL SCREEN", {{}, {}, {}, {}, u"Layar penuh"}},
    {"SCREEN FILTER", {{}, {}, {}, {}, u"Filter layar"}},
    {"SHARPEN", {{}, {}, {}, {}, u"Pertajam"}},
    {"VIVID", {{}, {}, {}, {}, u"Warna cerah"}},
    {"FPS COUNTER POSITION", {u"Position du compteur", u"Position der FPS-Anzeige", u"Posizione contatore FPS", u"Posición del contador", u"Posisi penghitung FPS"}},
    {"SHOW CPU / GPU USAGE", {u"Afficher l'utilisation CPU / GPU", u"CPU- / GPU-Auslastung anzeigen", u"Mostra utilizzo CPU / GPU", u"Mostrar uso de CPU / GPU", u"Tampilkan penggunaan CPU / GPU"}},
    {"SHOW TEMPERATURES", {u"Afficher les températures", u"Temperaturen anzeigen", u"Mostra temperature", u"Mostrar temperaturas", u"Tampilkan suhu"}},
    {"TOP CENTER", {u"En haut au centre", u"Oben mittig", u"In alto al centro", u"Arriba al centro", u"Atas tengah"}},
    {"TOP LEFT", {u"En haut à gauche", u"Oben links", u"In alto a sinistra", u"Arriba a la izquierda", u"Kiri atas"}},
    {"TOP RIGHT", {u"En haut à droite", u"Oben rechts", u"In alto a destra", u"Arriba a la derecha", u"Kanan atas"}},
    {"BOTTOM LEFT", {u"En bas à gauche", u"Unten links", u"In basso a sinistra", u"Abajo a la izquierda", u"Kiri bawah"}},
    {"BOTTOM CENTER", {u"En bas au centre", u"Unten mittig", u"In basso al centro", u"Abajo al centro", u"Bawah tengah"}},
    {"BOTTOM RIGHT", {u"En bas à droite", u"Unten rechts", u"In basso a destra", u"Abajo a la derecha", u"Kanan bawah"}},
    {"FPS COUNTER", {u"Compteur d'images par seconde", u"FPS-Anzeige", u"Contatore FPS", u"Contador de FPS", u"Penghitung FPS"}},
    {"ANISOTROPIC FILTERING", {u"Filtrage anisotropique", u"Anisotropische Filterung", u"Filtro anisotropico", u"Filtro anisotrópico", u"Filter anisotropik"}},
    {"LANGUAGE", {u"Langue", u"Sprache", u"Lingua", u"Idioma", u"Bahasa"}},
    {"AUDIO OUTPUT", {u"Sortie audio", u"Audioausgabe", u"Uscita audio", u"Salida de audio", u"Keluaran audio"}},
    {"CHEATS", {{}, {}, {}, {}, u"Cheat"}},
    {"ANDROID OPTIONS", {{}, {}, {}, {}, u"Opsi Android"}},
    {"INFINITE NITROUS", {{}, {}, {}, {}, u"Nitro tak terbatas"}},
    {"INFINITE SPEEDBREAKER", {{}, {}, {}, {}, u"Speedbreaker tak terbatas"}},
    {"MAX CASH", {{}, {}, {}, {}, u"Uang maksimal"}},
    {"NO POLICE", {{}, {}, {}, {}, u"Tanpa polisi"}},
    {"UNLOCK ALL", {{}, {}, {}, {}, u"Buka semua"}},
    {"SKIP MOVIES", {{}, {}, {}, {}, u"Lewati video"}},
    {"CONTROLS OPACITY", {{}, {}, {}, {}, u"Transparansi kontrol"}},
    {"STICK MODE", {{}, {}, {}, {}, u"Mode analog"}},
    {"FLOATING", {{}, {}, {}, {}, u"Mengambang"}},
    {"FIXED", {{}, {}, {}, {}, u"Tetap"}},
    {"CONTROLS SIZE", {{}, {}, {}, {}, u"Ukuran kontrol"}},
    {"SPEEDOMETER", {u"Compteur de vitesse", u"Tachometer", u"Tachimetro", u"Velocímetro", u"Spidometer"}},
    {"MINIMAP", {u"Mini-carte", u"Minikarte", u"Minimappa", u"Minimapa", u"Peta mini"}},
    {"RACE INFO", {u"Infos de course", u"Renninfo", u"Info gara", u"Info de carrera", u"Info balapan"}},
    {"EDIT CONTROLS POSITION", {{}, {}, {}, {}, u"Ubah posisi kontrol"}},
    {"STEERING", {{}, {}, {}, {}, u"Kemudi"}},
    {"ARROWS", {{}, {}, {}, {}, u"Panah"}},
    {"WHEEL", {{}, {}, {}, {}, u"Setir"}},
    {"TILT", {{}, {}, {}, {}, u"Miringkan HP"}},
    {"TILT ANGLE", {{}, {}, {}, {}, u"Sudut kemiringan"}},
    {"CREDITS", {u"Crédits", u"Mitwirkende", u"Crediti", u"Créditos", u"Kredit"}},
    {"ANDROID PORT", {u"Portage Android", u"Android-Portierung", u"Porting Android", u"Port de Android", u"Port Android"}},
    {"RECOMPILER AND RUNTIME", {u"Recompilateur et runtime", u"Recompiler und Laufzeit", u"Ricompilatore e runtime", u"Recompilador y runtime", u"Recompiler dan runtime"}},
    {"LIBRARIES", {u"Bibliothèques", u"Bibliotheken", u"Librerie", u"Bibliotecas", u"Pustaka"}},
    {"XBOX REFERENCES", {u"Références Xbox", u"Xbox-Referenzen", u"Riferimenti Xbox", u"Referencias de Xbox", u"Referensi Xbox"}},
    {"SUPPORT", {u"Soutenir", u"Unterstützen", u"Supporta", u"Apoyar", u"Dukungan"}},
    {"DONATE", {u"Faire un don", u"Spenden", u"Dona", u"Donar", u"Donasi"}},

    // their values
    {"FULLSCREEN", {u"Plein écran", u"Vollbild", u"Schermo intero", u"Pantalla completa", u"Layar penuh"}},
    {"WINDOWED", {u"Fenêtré", u"Fenster", u"Finestra", u"Ventana", u"Jendela"}},
    {"BORDERLESS", {u"Fenêtré sans bordure", u"Randloses Fenster", u"Finestra senza bordi", u"Ventana sin bordes", u"Jendela tanpa bingkai"}},
    {"ON", {u"Oui", u"Ein", u"Sì", u"Sí", u"Aktif"}},
    {"OFF", {u"Non", u"Aus", u"No", u"No", u"Mati"}},
    {"UNLIMITED", {u"Illimité", u"Unbegrenzt", u"Illimitato", u"Ilimitado", u"Tanpa batas"}},
    {"REFRESH RATE", {u"Fréquence de rafraîchissement", u"Bildwiederholfrequenz", u"Frequenza di aggiornamento", u"Frecuencia de actualización", u"Refresh rate layar"}},
    {"AUTOMATIC", {u"Automatique", u"Automatisch", u"Automatico", u"Automático", u"Otomatis"}},
    {"GAME DEFAULT", {u"Par défaut du jeu", u"Spielstandard", u"Predefinito del gioco", u"Predeterminado del juego", u"Bawaan game"}},
    {"NOT SUPPORTED", {u"Non pris en charge", u"Nicht unterstützt", u"Non supportato", u"No compatible", u"Tidak didukung"}},
    {"MONO", {u"Mono", u"Mono", u"Mono", u"Mono", u"Mono"}},
    {"2.0 STEREO", {u"2.0 Stéréo", u"2.0 Stereo", u"2.0 Stereo", u"2.0 Estéreo", u"2.0 Stereo"}},
    {"5.1 SURROUND", {u"5.1 Surround", u"5.1 Surround", u"5.1 Surround", u"5.1 Envolvente", u"5.1 Surround"}},
    {"(DEFAULT)", {u"(par défaut)", u"(Standard)", u"(predefinito)", u"(por defecto)", u"(bawaan)"}},
    {"(RESTART)", {u"(redémarrage)", u"(Neustart)", u"(riavvio)", u"(reinicio)", u"(mulai ulang)"}},
    {"ENGLISH", {u"Anglais", u"Englisch", u"Inglese", u"Inglés", u"Inggris"}},
    {"FRENCH", {u"Français", u"Französisch", u"Francese", u"Francés", u"Prancis"}},
    {"GERMAN", {u"Allemand", u"Deutsch", u"Tedesco", u"Alemán", u"Jerman"}},
    {"ITALIAN", {u"Italien", u"Italienisch", u"Italiano", u"Italiano", u"Italia"}},
    {"SPANISH", {u"Espagnol", u"Spanisch", u"Spagnolo", u"Español", u"Spanyol"}},
    // its own name in every language, so it can be found from any of them
    {"INDONESIAN", {u"Bahasa Indonesia", u"Bahasa Indonesia", u"Bahasa Indonesia", u"Bahasa Indonesia", u"Bahasa Indonesia"}},
    {"OPEN", {{}, {}, {}, {}, u"Buka"}},
    {"CHECK FOR UPDATES", {u"Rechercher des mises à jour", u"Nach Updates suchen", u"Cerca aggiornamenti", u"Buscar actualizaciones", u"Cek pembaruan"}},
    {"CHECK", {u"Rechercher", u"Suchen", u"Cerca", u"Buscar", u"Cek"}},
    {"DEFAULT", {{}, {}, {}, {}, u"Bawaan"}},

    {"SETTINGS PANEL", {u"Panneau des paramètres", u"Einstellungsmenü", u"Pannello impostazioni", u"Panel de configuración", u"Panel pengaturan"}},

    // the Xbox pad's inputs, the KEYBOARD and CONTROLLER pages' rows
    {"BLACK", {{}, {}, {}, {}, u"Tombol hitam"}},
    {"WHITE", {{}, {}, {}, {}, u"Tombol putih"}},
    {"LEFT TRIGGER", {{}, {}, {}, {}, u"Trigger kiri"}},
    {"RIGHT TRIGGER", {{}, {}, {}, {}, u"Trigger kanan"}},
    {"DPAD UP", {{}, {}, {}, {}, u"D-pad atas"}},
    {"DPAD DOWN", {{}, {}, {}, {}, u"D-pad bawah"}},
    {"DPAD LEFT", {{}, {}, {}, {}, u"D-pad kiri"}},
    {"DPAD RIGHT", {{}, {}, {}, {}, u"D-pad kanan"}},
    {"LEFT THUMB", {{}, {}, {}, {}, u"Tekan analog kiri"}},
    {"RIGHT THUMB", {{}, {}, {}, {}, u"Tekan analog kanan"}},
    {"LEFT STICK UP", {{}, {}, {}, {}, u"Analog kiri atas"}},
    {"LEFT STICK DOWN", {{}, {}, {}, {}, u"Analog kiri bawah"}},
    {"LEFT STICK LEFT", {{}, {}, {}, {}, u"Analog kiri ke kiri"}},
    {"LEFT STICK RIGHT", {{}, {}, {}, {}, u"Analog kiri ke kanan"}},
    {"RIGHT STICK UP", {{}, {}, {}, {}, u"Analog kanan atas"}},
    {"RIGHT STICK DOWN", {{}, {}, {}, {}, u"Analog kanan bawah"}},
    {"RIGHT STICK LEFT", {{}, {}, {}, {}, u"Analog kanan ke kiri"}},
    {"RIGHT STICK RIGHT", {{}, {}, {}, {}, u"Analog kanan ke kanan"}},

    // a hint's "PRESS (A) :" with the key in place of the button
    {"PRESS", {u"Appuyez sur", u"Drücke", u"Premi", u"Pulsa", u"Tekan"}},

    // the F10 panel's own
    {"SETTINGS", {u"Paramètres", u"Einstellungen", u"Impostazioni", u"Configuración", u"Pengaturan"}},
    {"BACK", {u"Retour", u"Zurück", u"Indietro", u"Atrás", u"Kembali"}},
    {"DEFAULTS", {u"Par défaut", u"Standard", u"Predefiniti", u"Por defecto", u"Bawaan"}},
    {"MOUSE", {u"Souris", u"Maus", u"Mouse", u"Ratón", u"Mouse"}},
    {"KEYBOARD", {u"Clavier", u"Tastatur", u"Tastiera", u"Teclado", u"Keyboard"}},
    {"Press a key or mouse button to bind, Escape cancels", {u"Appuyez sur une touche ou un bouton de souris, Échap pour annuler", u"Taste oder Maustaste drücken, Escape bricht ab", u"Premi un tasto o un pulsante del mouse, Esc per annullare", u"Pulsa una tecla o un botón del ratón, Escape para cancelar", u"Tekan tombol keyboard atau mouse untuk mengatur, Escape untuk batal"}},
    {"Right click or Delete clears the key", {u"Clic droit ou Suppr pour effacer la touche", u"Rechtsklick oder Entf löscht die Taste", u"Clic destro o Canc per cancellare il tasto", u"Clic derecho o Supr para borrar la tecla", u"Klik kanan atau Delete untuk menghapus tombol"}},
    {"CONTROLLER", {u"Manette", u"Controller", u"Controller", u"Mando", u"Controller"}},
    {"PRESS A BUTTON", {u"Appuyez sur un bouton", u"Taste drücken", u"Premi un pulsante", u"Pulsa un botón", u"Tekan tombol"}},
    {"Press a button on the controller to bind, wait 5 seconds to cancel", {u"Appuyez sur un bouton de la manette, attendez 5 secondes pour annuler", u"Controller-Taste drücken, 5 Sekunden warten bricht ab", u"Premi un pulsante del controller, attendi 5 secondi per annullare", u"Pulsa un botón del mando, espera 5 segundos para cancelar", u"Tekan tombol di controller untuk mengatur, tunggu 5 detik untuk batal"}},
    {"Right click or Delete restores the default", {u"Clic droit ou Suppr pour rétablir la valeur par défaut", u"Rechtsklick oder Entf stellt den Standard wieder her", u"Clic destro o Canc per ripristinare il predefinito", u"Clic derecho o Supr para restaurar el predeterminado", u"Klik kanan atau Delete untuk kembali ke bawaan"}},

    // the touch layout editor
    {"RESET", {{}, {}, {}, {}, u"Atur ulang"}},
    {"CANCEL", {{}, {}, {}, {}, u"Batal"}},
    {"HIDE", {{}, {}, {}, {}, u"Sembunyikan"}},
    {"SHOW", {{}, {}, {}, {}, u"Tampilkan"}},
    {"DONE", {{}, {}, {}, {}, u"Selesai"}},

    // a menu's A and B pills over the touch controls
    {"ENTER", {{}, {}, {}, {}, u"Pilih"}},
    {"CLOSE", {{}, {}, {}, {}, u"Tutup"}},

    // the message boxes before the game runs (native/). Whole sentences need every language: one with no text in
    // a language would have its words swapped one by one ("GAME", "ON", ...)
    {"is a different release of the game than this build was made from, and will most likely crash.\n\nStart it "
     "anyway?",
     {u"est une autre version du jeu que celle de cette build, et plantera très probablement.\n\nLe lancer quand "
      u"même ?",
      u"ist eine andere Version des Spiels als die, aus der dieser Build gemacht wurde, und stürzt sehr "
      u"wahrscheinlich ab.\n\nTrotzdem starten?",
      u"è una versione del gioco diversa da quella di questa build, e molto probabilmente andrà in crash.\n\n"
      u"Avviarlo comunque?",
      u"es una versión del juego distinta de la de esta build, y lo más probable es que se cierre.\n\n¿Iniciarlo "
      u"de todos modos?",
      u"adalah rilis game yang berbeda dari yang dipakai build ini, dan kemungkinan besar akan crash.\n\nTetap "
      u"jalankan?"}},
    {"YES", {u"Oui", u"Ja", u"Sì", u"Sí", u"Ya"}},
    {"NO", {u"Non", u"Nein", u"No", u"No", u"Tidak"}},
    {"The game failed to start, see the log in the logs folder.",
     {u"Le jeu n'a pas pu démarrer, voir le journal dans le dossier logs.",
      u"Das Spiel konnte nicht starten, siehe das Protokoll im Ordner logs.",
      u"Il gioco non è riuscito ad avviarsi, vedi il log nella cartella logs.",
      u"El juego no pudo iniciarse, consulta el registro en la carpeta logs.",
      u"Game gagal dimulai, lihat log di folder logs."}},
};

bool Same(std::string_view a, std::string_view b) {
    auto upper = [](char c) { return char(std::toupper(uint8_t(c))); };
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [&](char x, char y) {
               return upper(x) == upper(y);
           });
}

bool WordChar(char c) { return std::isalnum(uint8_t(c)) != 0; }

// the language the game booted in, -1 for English or one this disc cannot boot in
int Language() {
    static const int language = [] {
        if (!game::LanguageAvailable(game::BootLanguage())) return -1;
        auto at = std::find(std::begin(game::kLanguages), std::end(game::kLanguages), game::BootLanguage());
        return at == std::end(game::kLanguages) ? -1 : int(at - std::begin(game::kLanguages)) - 1;
    }();
    return language;
}

// null where the entry has none in this language, so the English shows
const char16_t* Translation(std::string_view english) {
    for (const Entry& e : kEntries)
        if (Same(e.english, english)) return e.text[Language()].empty() ? nullptr : e.text[Language()].data();
    return nullptr;
}

// the longest phrase with a translation from text[at], at a word start; end set past it
const char16_t* PhraseAt(std::string_view text, size_t at, size_t& end) {
    if (at && WordChar(text[at - 1])) return nullptr;
    for (end = text.size(); end > at; end--) {
        if (end < text.size() && WordChar(text[end])) continue;
        if (const char16_t* found = Translation(text.substr(at, end - at))) return found;
    }
    return nullptr;
}

// latin 1 lower case to upper; 0xF7 is the division sign
char16_t Upper(char16_t c) {
    bool lower = (c >= u'a' && c <= u'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7);
    return lower ? char16_t(c - 0x20) : c;
}

void AppendUtf8(std::string& out, char16_t c) {
    if (c < 0x80) {
        out += char(c);
    } else if (c < 0x800) {
        out += char(0xC0 | c >> 6);
        out += char(0x80 | (c & 0x3F));
    } else {
        out += char(0xE0 | c >> 12);
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    }
}

}  // namespace

std::string app::Ui(std::string_view text, bool caps) {
    if (Language() < 0) return std::string(text);
    // the panel's thread and the game's
    static std::mutex lock;
    static std::map<std::string, std::string> cache;
    std::lock_guard guard(lock);
    std::string key = std::string(text) + (caps ? "\1" : "\2");
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    std::string out;
    size_t i = 0;
    while (i < text.size()) {
        size_t end = 0;
        const char16_t* found = PhraseAt(text, i, end);
        if (!found) {
            out += text[i++];
            continue;
        }
        for (; *found; found++) AppendUtf8(out, caps ? Upper(*found) : *found);
        i = end;
    }
    return cache[key] = out;
}
