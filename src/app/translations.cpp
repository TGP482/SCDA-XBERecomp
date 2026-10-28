// Text the recomp adds to the game's menus in French, German, Italian and Spanish: Chaos Theory PC's wording where it
// had the same option or action, Double Agent's own names for its gadgets and visions (OPSAT is the montre-terminal
// in French and ENSAT in Spanish; Versus' from its controls page), and the game's casing, which capitalizes only the
// first word outside German.
#include <algorithm>
#include <cctype>
#include <iterator>

#include "app/app.h"
#include "app/translations.h"

namespace {

// in app::kLanguages order after int
struct Entry {
    std::string_view english;
    std::u16string_view text[4];
};
constexpr Entry kEntries[] = {
    // options menu pages and the RECOMP SETTINGS sections
    {"RECOMP SETTINGS", {u"Paramètres Recomp", u"Recomp-Einstellungen", u"Impostazioni Recomp", u"Configuración de Recomp"}},
    {"MOUSE AND KEYBOARD", {u"Souris et clavier", u"Maus und Tastatur", u"Mouse e tastiera", u"Ratón y teclado"}},
    {"CONTROLLER", {u"Manette", u"Controller", u"Controller", u"Controlador"}},
    {"GENERAL", {u"Général", u"Allgemein", u"Generale", u"General"}},
    {"DISPLAY", {u"Affichage", u"Anzeige", u"Video", u"Pantalla"}},
    {"GRAPHICS", {u"Graphismes", u"Grafik", u"Grafica", u"Gráficos"}},
    {"TEXTURES", {u"Textures", u"Texturen", u"Texture", u"Texturas"}},
    {"RESTORE DEFAULTS", {u"Rétablir les valeurs par défaut", u"Standard wiederherstellen", u"Ripristina predefiniti", u"Restaurar predeterminados"}},
    {"PRESS A KEY", {u"Appuyez sur une touche", u"Taste drücken", u"Premi un tasto", u"Pulsa una tecla"}},
    {"PRESS A BUTTON", {u"Appuyez sur un bouton", u"Taste drücken", u"Premi un pulsante", u"Pulsa un botón"}},
    {"UNBOUND", {u"Non assigné", u"Nicht belegt", u"Non assegnato", u"Sin asignar"}},

    // RECOMP SETTINGS rows
    {"MONITOR", {u"Écran", u"Monitor", u"Monitor", u"Monitor"}},
    {"DISPLAY MODE", {u"Mode d'affichage", u"Anzeigemodus", u"Modalità schermo", u"Modo de pantalla"}},
    {"RESOLUTION", {u"Résolution", u"Auflösung", u"Risoluzione", u"Resolución"}},
    {"RENDER RESOLUTION", {u"Résolution de rendu", u"Render-Auflösung", u"Risoluzione di rendering", u"Resolución de renderizado"}},
    {"FIELD OF VIEW", {u"Champ de vision", u"Sichtfeld", u"Campo visivo", u"Campo de visión"}},
    {"BRIGHTNESS", {u"Luminosité", u"Helligkeit", u"Luminosità", u"Brillo"}},
    {"CONTRAST", {u"Contraste", u"Kontrast", u"Contrasto", u"Contraste"}},
    {"VERTICAL SYNC", {u"Synchro verticale", u"V-Sync", u"Sincronia verticale", u"Sincronismo vertical"}},
    {"MAX FRAME RATE", {u"Images par seconde max.", u"Max. Bildrate", u"Frame rate massimo", u"Fotogramas por segundo máx."}},
    {"FPS COUNTER", {u"Compteur d'images par seconde", u"FPS-Anzeige", u"Contatore FPS", u"Contador de FPS"}},
    {"SHADER MODEL", {u"Modèle de shader", u"Shader Model", u"Modello di shader", u"Modelo de shader"}},
    {"HDR RENDERING", {u"Rendu HDR", u"HDR-Rendering", u"Rendering HDR", u"Renderizado HDR"}},
    {"FMV SCALE", {u"Échelle des cinématiques", u"Videoskalierung", u"Scala dei filmati", u"Escala de vídeos"}},
    {"FILL", {u"Remplir", u"Füllen", u"Riempi", u"Rellenar"}},
    {"COLOUR BLIND MODE", {u"Mode daltonien", u"Farbenblindmodus", u"Modalità daltonici", u"Modo daltónico"}},
    {"PROTANOPIA", {u"Protanopie", u"Protanopie", u"Protanopia", u"Protanopía"}},
    {"DEUTERANOPIA", {u"Deutéranopie", u"Deuteranopie", u"Deuteranopia", u"Deuteranopía"}},
    {"TRITANOPIA", {u"Tritanopie", u"Tritanopie", u"Tritanopia", u"Tritanopía"}},
    {"HDR OUTPUT", {u"Sortie HDR", u"HDR-Ausgabe", u"Uscita HDR", u"Salida HDR"}},
    {"TONE MAPPING", {u"Assignation des teintes", u"Tone Mapping", u"Tone mapping", u"Mapeado por tonos"}},
    {"AMBIENT OCCLUSION", {u"Occlusion ambiante", u"Ambient Occlusion", u"Occlusione ambientale", u"Oclusión ambiental"}},
    {"ANISOTROPIC FILTERING", {u"Filtrage anisotropique", u"Anisotropische Filterung", u"Filtro anisotropico", u"Filtro anisotrópico"}},
    {"SHADOW RESOLUTION", {u"Résolution des ombres", u"Schattenauflösung", u"Risoluzione delle ombre", u"Resolución de sombras"}},
    {"SHADOW DISTANCE", {u"Distance des ombres", u"Schattendistanz", u"Distanza delle ombre", u"Distancia de sombras"}},
    {"SHADOW FILTER", {u"Filtre des ombres", u"Schattenfilter", u"Filtro delle ombre", u"Filtro de sombras"}},
    {"TEXTURE REPLACEMENT", {u"Remplacement des textures", u"Texturersetzung", u"Sostituzione delle texture", u"Reemplazo de texturas"}},
    {"TEXTURE DUMPING", {u"Extraction des textures", u"Texturextraktion", u"Estrazione delle texture", u"Extracción de texturas"}},
    {"DUMP FORMAT", {u"Format d'extraction", u"Extraktionsformat", u"Formato di estrazione", u"Formato de extracción"}},
    {"SKIP INTROS", {u"Passer les intros", u"Intros überspringen", u"Salta introduzione", u"Saltar introducción"}},
    {"PAUSE ON FOCUS LOSS", {u"Pause en arrière-plan", u"Pause im Hintergrund", u"Pausa in background", u"Pausa en segundo plano"}},
    {"LANGUAGE", {u"Langue", u"Sprache", u"Lingua", u"Idioma"}},
    {"AUDIO OUTPUT", {u"Sortie audio", u"Audioausgabe", u"Uscita audio", u"Salida de audio"}},
    {"COOP TICK RATE", {u"Fréquence de mise à jour co-op", u"Koop-Tickrate", u"Frequenza di aggiornamento cooperativa", u"Tasa de actualización coop."}},
    {"COOP MIN INPUT DELAY", {u"Délai d'entrée min. co-op", u"Min. Koop-Eingabeverzögerung", u"Ritardo input min. cooperativa", u"Retardo de entrada mín. coop."}},

    // their values
    {"FULLSCREEN", {u"Plein écran", u"Vollbild", u"Schermo intero", u"Pantalla completa"}},
    {"WINDOWED", {u"Fenêtré", u"Fenster", u"Finestra", u"Ventana"}},
    {"BORDERLESS", {u"Fenêtré sans bordure", u"Randloses Fenster", u"Finestra senza bordi", u"Ventana sin bordes"}},
    {"ON", {u"Oui", u"Ein", u"Sì", u"Sí"}},
    {"OFF", {u"Non", u"Aus", u"No", u"No"}},
    {"UNLIMITED", {u"Illimité", u"Unbegrenzt", u"Illimitato", u"Ilimitado"}},
    {"REFRESH RATE", {u"Fréquence de rafraîchissement", u"Bildwiederholfrequenz", u"Frequenza di aggiornamento", u"Frecuencia de actualización"}},
    {"AUTOMATIC", {u"Automatique", u"Automatisch", u"Automatico", u"Automático"}},
    {"SM3 SOFT", {u"SM3 douce", u"SM3 weich", u"SM3 morbida", u"SM3 suave"}},
    {"NOT SUPPORTED", {u"Non pris en charge", u"Nicht unterstützt", u"Non supportato", u"No compatible"}},
    {"MONO", {u"Mono", u"Mono", u"Mono", u"Mono"}},
    {"2.0 STEREO", {u"2.0 Stéréo", u"2.0 Stereo", u"2.0 Stereo", u"2.0 Estéreo"}},
    {"5.1 SURROUND", {u"5.1 Surround", u"5.1 Surround", u"5.1 Surround", u"5.1 Envolvente"}},
    {"(DEFAULT)", {u"(par défaut)", u"(Standard)", u"(predefinito)", u"(por defecto)"}},
    {"(RESTART)", {u"(redémarrage)", u"(Neustart)", u"(riavvio)", u"(reinicio)"}},
    {"(PRIMARY)", {u"(principal)", u"(primär)", u"(principale)", u"(principal)"}},
    {"ENGLISH", {u"Anglais", u"Englisch", u"Inglese", u"Inglés"}},
    {"FRENCH", {u"Français", u"Französisch", u"Francese", u"Francés"}},
    {"GERMAN", {u"Allemand", u"Deutsch", u"Tedesco", u"Alemán"}},
    {"ITALIAN", {u"Italien", u"Italienisch", u"Italiano", u"Italiano"}},
    {"SPANISH", {u"Espagnol", u"Spanisch", u"Spagnolo", u"Español"}},

    // MOUSE AND KEYBOARD and CONTROLLER rows
    {"INVERT X AXIS", {u"Inverser l'axe X", u"X-Achse umkehren", u"Inverti asse X", u"Invertir eje X"}},
    {"INVERT Y AXIS", {u"Inverser l'axe Y", u"Y-Achse umkehren", u"Inverti asse Y", u"Invertir eje Y"}},
    {"FREE LOOK SENSITIVITY", {u"Sensibilité vue libre", u"Empfindlichkeit freie Sicht", u"Sensibilità visuale libera", u"Sensibilidad vista libre"}},
    {"WEAPON SENSITIVITY", {u"Sensibilité arme", u"Empfindlichkeit Waffe", u"Sensibilità arma", u"Sensibilidad arma"}},
    {"AIM HORIZONTAL SENSITIVITY", {u"Sensibilité horizontale en visée", u"Horizontale Zielempfindlichkeit", u"Sensibilità orizzontale in mira", u"Sensibilidad horizontal al apuntar"}},
    {"AIM VERTICAL SENSITIVITY", {u"Sensibilité verticale en visée", u"Vertikale Zielempfindlichkeit", u"Sensibilità verticale in mira", u"Sensibilidad vertical al apuntar"}},
    {"SNIPER SENSITIVITY", {u"Sensibilité sniper", u"Empfindlichkeit Scharfschütze", u"Sensibilità cecchino", u"Sensibilidad francotirador"}},
    {"MOVE FORWARD", {u"Avancer", u"Vorwärts bewegen", u"Avanti", u"Avanzar"}},
    {"MOVE BACK", {u"Reculer", u"Rückwärts bewegen", u"Indietro", u"Retroceder"}},
    {"MOVE LEFT", {u"Déplacement à gauche", u"Nach links", u"Spostati a sinistra", u"Mover a la izquierda"}},
    {"MOVE RIGHT", {u"Déplacement à droite", u"Nach rechts", u"Spostati a destra", u"Mover a la derecha"}},
    {"JUMP", {u"Sauter", u"Springen", u"Salta", u"Saltar"}},
    {"JUMP / SPLIT JUMP", {u"Sauter / saut-écart", u"Springen / Spagatsprung", u"Salta / Spaccata", u"Saltar / Salto de piernas abiertas"}},
    {"CROUCH", {u"S'accroupir", u"Ducken", u"Chinati", u"Agacharse"}},
    {"ZOOM IN / ACCELERATE", {u"Zoom avant / accélérer", u"Heranzoomen / Beschleunigen", u"Zoom avanti / Accelera", u"Acercar / Acelerar"}},
    {"ZOOM OUT / DECELERATE", {u"Zoom arrière / ralentir", u"Herauszoomen / Verlangsamen", u"Zoom indietro / Rallenta", u"Alejar / Decelerar"}},
    {"CROUCH / STAND UP / EXIT INTERACTION", {u"S'accroupir / se relever / quitter l'interaction", u"Ducken / Aufstehen / Interaktion beenden", u"Chinati / Alzati / Esci dall'interazione", u"Agacharse / De pie / Salir de la acción"}},
    {"BACK TO WALL / SWITCH HAND", {u"Dos au mur / changer de main", u"An Wand lehnen / Hand wechseln", u"Spalle al muro / Cambia mano", u"Espalda a la pared / Cambiar de mano"}},
    {"BACK TO WALL / SWITCH HAND / ZOOM IN OUT", {u"Dos au mur / changer de main / zoom", u"An Wand lehnen / Hand wechseln / Zoom", u"Spalle al muro / Cambia mano / Zoom", u"Espalda a la pared / Cambiar de mano / Zoom"}},
    {"FIRE", {u"Tirer", u"Feuern", u"Fuoco", u"Disparar"}},
    {"ALTERNATE FIRE", {u"Tir secondaire", u"Sekundärfeuer", u"Fuoco alternativo", u"Disparo secundario"}},
    {"INTERACTION", {u"Interaction", u"Interaktion", u"Interazione", u"Acciones"}},
    {"INTERACTION / RELOAD", {u"Interaction / recharger", u"Interaktion / Nachladen", u"Interazione / Ricarica", u"Acciones / Recargar"}},
    {"USE / EQUIP", {u"Utiliser / s'équiper", u"Benutzen / Ausrüsten", u"Usa / Impugna", u"Usar / Empuñar"}},
    {"RELOAD", {u"Recharger", u"Nachladen", u"Ricarica", u"Recargar"}},
    {"WHISTLE", {u"Siffler", u"Pfeifen", u"Fischia", u"Silbar"}},
    {"WHISTLE / COOP ACTION", {u"Siffler / action coop.", u"Pfeifen / Koop-Aktion", u"Fischia / Azione cooperativa", u"Silbar / Acción coop."}},
    {"COOP ACTION", {u"Action coopérative", u"Koop-Aktion", u"Azione cooperativa", u"Acción cooperativa"}},
    {"QUICK INVENTORY", {u"Inventaire rapide", u"Schnellinventar", u"Inventario rapido", u"Inventario rápido"}},
    {"OPSAT", {u"Montre-terminal", u"OPSAT", u"OPSAT", u"ENSAT"}},
    {"EEV VISION / ZOOM / SNIPER", {u"Vision EEV / zoom / sniper", u"EEV / Zoom / Scharfschütze", u"EEV / Zoom / Mirino", u"Visor VE / Zoom / Francotirador"}},
    {"NIGHT VISION", {u"Vision nocturne", u"Nachtsicht", u"Visore notturno", u"Visor nocturno"}},
    {"THERMAL VISION", {u"Vision thermique", u"Wärmebildsicht", u"Visore termico", u"Visor térmico"}},
    {"EMF VISION", {u"Vision CES", u"EMF-Sicht", u"Visore campo magnetico", u"Visor CEM"}},
    {"STICKY CAM VISION", {u"Vision par caméra-glu", u"Haftkamera-Sicht", u"Visuale dalla microcamera", u"Visor cámara seguimiento"}},
    {"PUSH TO TALK", {u"Appuyer pour parler", u"Push-to-Talk", u"Premi per parlare", u"Pulsar para hablar"}},
    {"PAUSE", {u"Pause", u"Pause", u"Pausa", u"Pausa"}},
    {"QUICK SAVE", {u"Sauvegarde rapide", u"Schnellspeichern", u"Salvataggio veloce", u"Guardado rápido"}},
    {"QUICK LOAD", {u"Chargement rapide", u"Schnellladen", u"Caricamento veloce", u"Carga rápida"}},
    {"CONSOLE", {u"Console", u"Konsole", u"Console", u"Consola"}},
    {"TOGGLE HUD", {u"Afficher / masquer l'interface", u"HUD ein / aus", u"Mostra / nascondi HUD", u"Mostrar / ocultar HUD"}},
    {"TOGGLE TEXTURE REPLACEMENT", {u"Activer / désactiver le remplacement des textures", u"Texturersetzung ein / aus", u"Attiva / disattiva sostituzione delle texture", u"Activar / desactivar reemplazo de texturas"}},
    {"SETTINGS PANEL", {u"Panneau des paramètres", u"Einstellungsmenü", u"Pannello impostazioni", u"Panel de configuración"}},

    // Versus keys
    {"INTERACT", {u"Action", u"Aktion", u"Azione", u"Acción"}},
    {"JUMP / RELOAD", {u"Sauter / recharger", u"Springen / Nachladen", u"Salta / Ricarica", u"Saltar / Recargar"}},
    {"ZOOM / LOOK BACK", {u"Zoom / regarder derrière", u"Zoom / Nach hinten sehen", u"Zoom / Guarda indietro", u"Zoom / Mirar atrás"}},
    {"LASER", {u"Laser de visée", u"Laser-Sicht", u"Mirino laser", u"Mira láser"}},
    {"PREVIOUS CAMERA", {u"Caméra précédente", u"Vorige Haftkamera", u"Stickycam precedente", u"Cámara anterior"}},
    {"HELP", {u"Texte d'aide", u"Hilfetext", u"Guida", u"Ayuda"}},

    // the F10 panel's own
    {"SETTINGS", {u"Paramètres", u"Einstellungen", u"Impostazioni", u"Configuración"}},
    {"BACK", {u"Retour", u"Zurück", u"Indietro", u"Atrás"}},
    {"DEFAULTS", {u"Par défaut", u"Standard", u"Predefiniti", u"Por defecto"}},
    {"APPLY", {u"Appliquer", u"Übernehmen", u"Applica", u"Aplicar"}},
    {"MOUSE", {u"Souris", u"Maus", u"Mouse", u"Ratón"}},
    {"KEYBOARD", {u"Clavier", u"Tastatur", u"Tastiera", u"Teclado"}},
    {"HORIZONTAL SENSITIVITY", {u"Sensibilité horizontale", u"Horizontale Empfindlichkeit", u"Sensibilità orizzontale", u"Sensibilidad horizontal"}},
    {"VERTICAL SENSITIVITY", {u"Sensibilité verticale", u"Vertikale Empfindlichkeit", u"Sensibilità verticale", u"Sensibilidad vertical"}},
    {"VIBRATION STRENGTH", {u"Intensité des vibrations", u"Vibrationsstärke", u"Intensità vibrazione", u"Intensidad de la vibración"}},
    {"DEADZONE", {u"Zone morte", u"Totzone", u"Zona morta", u"Zona muerta"}},
    {"GYRO", {u"Gyroscope", u"Gyroskop", u"Giroscopio", u"Giroscopio"}},
    {"GYRO AIMING", {u"Visée gyroscopique", u"Gyro-Zielen", u"Mira giroscopica", u"Apuntado con giroscopio"}},
    {"GYRO SENSITIVITY", {u"Sensibilité du gyroscope", u"Gyro-Empfindlichkeit", u"Sensibilità del giroscopio", u"Sensibilidad del giroscopio"}},
    {"GYRO AXIS", {u"Axe du gyroscope", u"Gyro-Achse", u"Asse del giroscopio", u"Eje del giroscopio"}},
    {"GYRO BUTTON", {u"Bouton du gyroscope", u"Gyro-Taste", u"Pulsante del giroscopio", u"Botón del giroscopio"}},
    {"GYRO BUTTON MODE", {u"Mode du bouton du gyroscope", u"Gyro-Tastenmodus", u"Modalità pulsante del giroscopio", u"Modo del botón del giroscopio"}},
    {"RECENTER BUTTON", {u"Bouton de recentrage", u"Zentrieren-Taste", u"Pulsante di ricentratura", u"Botón de recentrado"}},
    {"CALIBRATE GYRO", {u"Calibrer le gyroscope", u"Gyro kalibrieren", u"Calibra il giroscopio", u"Calibrar el giroscopio"}},
    {"YAW", {u"Lacet", u"Gieren", u"Imbardata", u"Guiñada"}},
    {"ROLL", {u"Roulis", u"Rollen", u"Rollio", u"Alabeo"}},
    {"WORLD", {u"Monde", u"Welt", u"Mondo", u"Mundo"}},
    {"NONE", {u"Aucun", u"Keine", u"Nessuno", u"Ninguno"}},
    {"TOUCHPAD", {u"Pavé tactile", u"Touchpad", u"Touchpad", u"Panel táctil"}},
    {"MISC", {u"Divers", u"Sonstige", u"Varie", u"Varios"}},
    {"RIGHT PADDLE 1", {u"Palette droite 1", u"Paddle rechts 1", u"Paletta destra 1", u"Palanca derecha 1"}},
    {"LEFT PADDLE 1", {u"Palette gauche 1", u"Paddle links 1", u"Paletta sinistra 1", u"Palanca izquierda 1"}},
    {"RIGHT PADDLE 2", {u"Palette droite 2", u"Paddle rechts 2", u"Paletta destra 2", u"Palanca derecha 2"}},
    {"LEFT PADDLE 2", {u"Palette gauche 2", u"Paddle links 2", u"Paletta sinistra 2", u"Palanca izquierda 2"}},
    {"OFF WHILE HELD", {u"Désactivé si maintenu", u"Aus beim Halten", u"Disattivato se tenuto premuto", u"Desactivado al mantener"}},
    {"ON WHILE HELD", {u"Activé si maintenu", u"Ein beim Halten", u"Attivato se tenuto premuto", u"Activado al mantener"}},
    {"TOGGLE", {u"Basculer", u"Umschalten", u"Alterna", u"Alternar"}},
    {"START", {u"Démarrer", u"Starten", u"Avvia", u"Iniciar"}},
    {"KEEP STILL", {u"Ne bougez pas", u"Nicht bewegen", u"Non muovere", u"No lo muevas"}},
    {"BUTTONS", {u"Boutons", u"Tasten", u"Pulsanti", u"Botones"}},
    {"INPUT DEVICES", {u"Périphériques", u"Eingabegeräte", u"Dispositivi di input", u"Dispositivos de entrada"}},
    {"PLAYER 1", {u"Joueur 1", u"Spieler 1", u"Giocatore 1", u"Jugador 1"}},
    {"PLAYER 2", {u"Joueur 2", u"Spieler 2", u"Giocatore 2", u"Jugador 2"}},
    {"(FROM PLAYER 1)", {u"(du joueur 1)", u"(von Spieler 1)", u"(dal giocatore 1)", u"(del jugador 1)"}},
    {"(FROM PLAYER 2)", {u"(du joueur 2)", u"(von Spieler 2)", u"(dal giocatore 2)", u"(del jugador 2)"}},
    {"NOT CONNECTED", {u"Non connecté", u"Nicht verbunden", u"Non collegato", u"No conectado"}},
    {"(UNPLUGGED)", {u"(débranché)", u"(entfernt)", u"(scollegato)", u"(desconectado)"}},
    {"MICROPHONE", {u"Microphone", u"Mikrofon", u"Microfono", u"Micrófono"}},
    {"SYSTEM DEFAULT", {u"Par défaut du système", u"Systemstandard", u"Predefinito di sistema", u"Predeterminado del sistema"}},
    {"COOP JOIN IP", {u"IP de connexion co-op", u"Koop-Beitritts-IP", u"IP per unirsi in cooperativa", u"IP para unirse en coop."}},
    {"VERSUS JOIN IP", {u"IP de connexion Versus", u"Versus-Beitritts-IP", u"IP per unirsi in Versus", u"IP para unirse en Versus"}},
    {"Press a key or mouse button to bind, Escape cancels", {u"Appuyez sur une touche ou un bouton de souris, Échap pour annuler", u"Taste oder Maustaste drücken, Escape bricht ab", u"Premi un tasto o un pulsante del mouse, Esc per annullare", u"Pulsa una tecla o un botón del ratón, Escape para cancelar"}},
    {"Right click or Delete clears the key", {u"Clic droit ou Suppr pour effacer la touche", u"Rechtsklick oder Entf löscht die Taste", u"Clic destro o Canc per cancellare il tasto", u"Clic derecho o Supr para borrar la tecla"}},
    {"Press a controller button to bind, Escape cancels", {u"Appuyez sur un bouton de la manette, Échap pour annuler", u"Controller-Taste drücken, Escape bricht ab", u"Premi un pulsante del controller, Esc per annullare", u"Pulsa un botón del controlador, Escape para cancelar"}},

    // Exit on the game mode menu
    {"EXIT", {u"Sortir", u"Verlassen", u"Esci", u"Salir"}},
    {"QUIT GAME", {u"Quitter la partie", u"Spiel beenden", u"Esci dal gioco", u"Salir del juego"}},
    {"ARE YOU SURE YOU WANT TO EXIT?", {u"Voulez-vous vraiment quitter?", u"Möchten Sie das Spiel wirklich beenden?", u"Vuoi davvero uscire?", u"¿Seguro que quieres salir?"}},

    // game text replaced by key (text.cpp)
    {"LAN", {u"LAN", u"LAN", u"Rete locale", u"En red"}},
    {"PLEASE DON'T CLOSE THE GAME OR TURN OFF YOUR PC.", {u"Ne fermez pas le jeu et n'éteignez pas votre PC.", u"Bitte beenden Sie das Spiel nicht und schalten Sie Ihren PC nicht aus.", u"Non chiudere il gioco e non spegnere il PC.", u"No cierres el juego ni apagues el PC."}},
    {"YOUR PC DOESN'T HAVE ENOUGH FREE STORAGE TO SAVE A PROFILE.", {u"Votre PC ne dispose pas de suffisamment d'espace libre pour sauvegarder un profil.", u"Ihr PC verfügt nicht über genug freien Speicherplatz, um ein Profil zu speichern.", u"Il PC non dispone di spazio libero sufficiente per salvare un profilo.", u"Tu PC no tiene suficiente espacio libre para guardar un perfil."}},
    {"YOUR PC DOESN'T HAVE ENOUGH FREE STORAGE TO SAVE THE CHECKPOINT.", {u"Votre PC ne dispose pas de suffisamment d'espace libre pour sauvegarder ce point de passage.", u"Ihr PC verfügt nicht über genug freien Speicherplatz, um diesen Speicherpunkt zu sichern.", u"Il PC non dispone di spazio libero sufficiente per salvare il checkpoint.", u"Tu PC no tiene suficiente espacio libre para guardar el checkpoint."}},
    {"YOUR PC DOESN'T HAVE ENOUGH FREE STORAGE TO SAVE THE CHECKPOINTS.", {u"Votre PC ne dispose pas de suffisamment d'espace libre pour sauvegarder les points de passage.", u"Ihr PC verfügt nicht über genug freien Speicherplatz, um die Speicherpunkte zu sichern.", u"Il PC non dispone di spazio libero sufficiente per salvare i checkpoint.", u"Tu PC no tiene suficiente espacio libre para guardar los checkpoints."}},
    // each language has the thermal and night vision hints the wrong way round too
    {"USE: PRESS RIGHT ON THE D-PAD.", {u"UTILISATION: appuyez à droite sur le BMD.", u"VERWENDEN: Rechts auf dem Steuerkreuz drücken.", u"USO: premi destra sul tasto direzionale.", u"USO: Presiona el mando de dirección hacia la derecha."}},
    {"USE: PRESS LEFT ON THE D-PAD.", {u"UTILISATION: appuyez à gauche sur le BMD.", u"VERWENDEN: Links auf dem Steuerkreuz drücken.", u"USO: premi sinistra sul tasto direzionale.", u"USO: Presiona el mando de dirección hacia la izquierda."}},
};

bool Same(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return (x >= 'a' && x <= 'z' ? x - 0x20 : x) == (y >= 'a' && y <= 'z' ? y - 0x20 : y);
           });
}

bool WordChar(char c) { return std::isalnum(uint8_t(c)) != 0; }

// the language the game booted with, -1 for English
int Language() {
    static const int language = [] {
        auto at = std::find(std::begin(app::kLanguages), std::end(app::kLanguages), app::BootLanguage());
        return at == std::end(app::kLanguages) ? -1 : int(at - std::begin(app::kLanguages)) - 1;
    }();
    return language;
}

}  // namespace

const char16_t* app::Translation(std::string_view english) {
    int language = Language();
    if (language < 0) return nullptr;
    for (const Entry& e : kEntries)
        if (Same(e.english, english)) return e.text[language].data();
    return nullptr;
}

std::u16string app::Translate(std::string_view text, std::string (*rest)(std::string_view)) {
    std::u16string out;
    size_t plain = 0;  // start of the text not yet out
    auto flush = [&](size_t to) {
        std::string kept = rest(text.substr(plain, to - plain));
        out.append(kept.begin(), kept.end());
    };
    for (size_t i = 0; i < text.size();) {
        const char16_t* found = nullptr;
        size_t end = text.size();
        if (!i || !WordChar(text[i - 1]))
            for (; end > i; end--)
                if ((end == text.size() || !WordChar(text[end])) && (found = Translation(text.substr(i, end - i))))
                    break;
        if (!found) {
            i++;
            continue;
        }
        flush(i);
        out += found;
        i = plain = end;
    }
    flush(text.size());
    return out;
}

// the F10 panel's upper case in the game's title case: "DISPLAY MODE" as "Display Mode", "SPACE/ENTER" as "Space/Enter"
std::string app::TitleCase(std::string_view text) {
    constexpr std::string_view kSmall[] = {"AND", "OR", "OF", "TO", "A"};
    constexpr std::string_view kKeep[] = {"FPS", "FMV", "EEV", "EMF", "LMB", "RMB", "MMB", "IP", "OPSAT", "ESC", "HDR",
                                          "MSAA", "SMAA", "SSAO", "GTAO", "HUD", "LB", "RB", "LT", "RT", "LS", "RS",
                                          "PNG", "DDS"};
    std::string out;
    size_t i = 0;
    while (i < text.size()) {
        size_t end = text.find_first_of(" /()", i);
        if (end == std::string_view::npos) end = text.size();
        std::string word(text.substr(i, end - i));
        bool digit = word.find_first_of("0123456789") != std::string::npos;
        if (word == "HZ") word = "Hz";
        else if (word == "D-PAD") word = "D-Pad";
        else if (word == "MS") word = "ms";
        else if (!digit && std::find(std::begin(kKeep), std::end(kKeep), word) == std::end(kKeep)) {
            bool small = !out.empty() && std::find(std::begin(kSmall), std::end(kSmall), word) != std::end(kSmall);
            for (size_t k = 0; k < word.size(); k++)
                if (small || k) word[k] = char(std::tolower(uint8_t(word[k])));
        }
        out += word;
        if (end < text.size()) out += text[end];
        i = end + 1;
    }
    return out;
}
