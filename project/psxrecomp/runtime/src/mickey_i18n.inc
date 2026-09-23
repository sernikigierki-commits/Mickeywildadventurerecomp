/* MICKEY_RA_I18N_V1_TABLE */
struct MickeyTrRow { const char* key; const char* v[5]; };
static const MickeyTrRow kMickeyTr[] = {
    {"main_new", {"NOWA GRA","NEW GAME","NUOVA PARTITA","ニューゲーム","NEUES SPIEL"}},
    {"main_continue", {"KONTYNUUJ","CONTINUE","CONTINUA","つづきから","FORTSETZEN"}},
    {"main_ach", {"OSIĄGNIĘCIA","ACHIEVEMENTS","OBIETTIVI","実績","ERFOLGE"}},
    {"main_settings", {"USTAWIENIA","SETTINGS","IMPOSTAZIONI","設定","EINSTELLUNGEN"}},
    {"main_exit", {"WYJŚCIE","EXIT","ESCI","終了","BEENDEN"}},
    {"settings_title", {"USTAWIENIA","SETTINGS","IMPOSTAZIONI","設定","EINSTELLUNGEN"}},
    {"bezel", {"RAMKA","BEZEL","CORNICE","フレーム","RAHMEN"}},
    {"crt", {"FILTR CRT","CRT FILTER","FILTRO CRT","CRTフィルター","CRT-FILTER"}},
    {"resolution", {"ROZDZIELCZOŚĆ","RESOLUTION","RISOLUZIONE","解像度","AUFLÖSUNG"}},
    {"window_mode", {"TRYB OKNA","WINDOW MODE","MODALITÀ FINESTRA","画面モード","FENSTERMODUS"}},
    {"language", {"JĘZYK","LANGUAGE","LINGUA","言語","SPRACHE"}},
    {"back", {"POWRÓT","BACK","INDIETRO","戻る","ZURÜCK"}},
    {"none", {"BRAK","NONE","NESSUNA","なし","KEIN"}},
    {"windowed", {"OKNO","WINDOWED","FINESTRA","ウィンドウ","FENSTER"}},
    {"borderless", {"BEZ RAMKI","BORDERLESS","SENZA BORDI","ボーダーレス","RAHMENLOS"}},
    {"fullscreen", {"PEŁNY EKRAN","FULLSCREEN","SCHERMO INTERO","フルスクリーン","VOLLBILD"}},
    {"desktop", {"PULPIT","DESKTOP","DESKTOP","デスクトップ","DESKTOP"}},
    {"preview", {"PODGLĄD NA ŻYWO","LIVE PREVIEW","ANTEPRIMA LIVE","ライブプレビュー","LIVE-VORSCHAU"}},
    {"hint_lr", {"LEWO / PRAWO = ZMIEŃ","LEFT / RIGHT = CHANGE","SINISTRA / DESTRA = CAMBIA","左右 = 変更","LINKS / RECHTS = ÄNDERN"}},
    {"profile_new", {"WYBIERZ PROFIL — NOWA GRA","SELECT PROFILE — NEW GAME","SELEZIONA PROFILO — NUOVA PARTITA","プロフィール選択 — ニューゲーム","PROFIL WÄHLEN — NEUES SPIEL"}},
    {"profile_continue", {"WYBIERZ PROFIL — KONTYNUUJ","SELECT PROFILE — CONTINUE","SELEZIONA PROFILO — CONTINUA","プロフィール選択 — つづきから","PROFIL WÄHLEN — FORTSETZEN"}},
    {"profile_hint", {"ENTER = WYBIERZ   DELETE = USUŃ   ESC = POWRÓT","ENTER = SELECT   DELETE = DELETE   ESC = BACK","INVIO = SELEZIONA   CANC = ELIMINA   ESC = INDIETRO","ENTER = 選択   DELETE = 削除   ESC = 戻る","ENTER = WÄHLEN   ENTF = LÖSCHEN   ESC = ZURÜCK"}},
    {"empty", {"PUSTY ZAPIS","EMPTY SLOT","SLOT VUOTO","空きスロット","LEERER SPEICHER"}},
    {"no_save", {"BRAK ZAPISU","NO SAVE","NESSUN SALVATAGGIO","セーブなし","KEIN SPIELSTAND"}},
    {"overwrite", {"ENTER NADPISZE TEN ZAPIS","ENTER OVERWRITES THIS SLOT","INVIO SOVRASCRIVE QUESTO SLOT","ENTERで上書きします","ENTER ÜBERSCHREIBT DIESEN SPEICHER"}},
    {"lives", {"ŻYCIA","LIVES","VITE","ライフ","LEBEN"}},
    {"marbles", {"KULKI","MARBLES","BIGLIE","ビー玉","MURMELN"}},
    {"pause", {"PAUZA","PAUSE","PAUSA","ポーズ","PAUSE"}},
    {"resume", {"WZNÓW GRĘ","RESUME","RIPRENDI","ゲームに戻る","FORTSETZEN"}},
    {"restart_level", {"RESTART POZIOMU","RESTART LEVEL","RIAVVIA LIVELLO","レベルをやり直す","LEVEL NEUSTARTEN"}},
    {"options", {"OPCJE","OPTIONS","OPZIONI","オプション","OPTIONEN"}},
    {"exit_game", {"WYJŚCIE Z GRY","EXIT GAME","ESCI DAL GIOCO","ゲームを終了","SPIEL VERLASSEN"}},
    {"practice_nosave", {"PRACTICE: BRAK ZAPISU — RESTART NIEDOSTĘPNY","PRACTICE: NO SAVE — RESTART UNAVAILABLE","PRATICA: NESSUN SALVATAGGIO — RIAVVIO NON DISPONIBILE","プラクティス：セーブなし — リスタート不可","ÜBUNG: KEIN SPEICHERN — NEUSTART NICHT VERFÜGBAR"}},
    {"checkpoint_not_ready", {"CHECKPOINT POZIOMU NIE JEST JESZCZE GOTOWY","LEVEL CHECKPOINT IS NOT READY YET","IL CHECKPOINT DEL LIVELLO NON È ANCORA PRONTO","レベルのチェックポイントはまだ準備できていません","LEVEL-CHECKPOINT IST NOCH NICHT BEREIT"}},
    {"esc_resume", {"ESC / B = WZNÓW","ESC / B = RESUME","ESC / B = RIPRENDI","ESC / B = 戻る","ESC / B = FORTSETZEN"}},
    {"live_apply", {"RAMKA I CRT SĄ STOSOWANE OD RAZU","BEZEL AND CRT APPLY IMMEDIATELY","CORNICE E CRT SI APPLICANO SUBITO","フレームとCRTはすぐ反映されます","RAHMEN UND CRT WERDEN SOFORT ANGEWENDET"}},
    {"back_pause", {"ESC / B = POWRÓT DO PAUZY","ESC / B = BACK TO PAUSE","ESC / B = TORNA ALLA PAUSA","ESC / B = ポーズへ戻る","ESC / B = ZURÜCK ZUR PAUSE"}},
    {"confirm_exit", {"CZY NA PEWNO CHCESZ WYJŚĆ Z GRY?","ARE YOU SURE YOU WANT TO EXIT THE GAME?","VUOI DAVVERO USCIRE DAL GIOCO?","本当にゲームを終了しますか？","MÖCHTEST DU DAS SPIEL WIRKLICH VERLASSEN?"}},
    {"unsaved", {"DOTYCHCZASOWY POSTĘP NIE ZOSTANIE ZAPISANY","CURRENT PROGRESS WILL NOT BE SAVED","I PROGRESSI ATTUALI NON SARANNO SALVATI","現在の進行状況は保存されません","DER AKTUELLE FORTSCHRITT WIRD NICHT GESPEICHERT"}},
    {"confirm_restart_level", {"CZY NA PEWNO CHCESZ ZRESETOWAĆ POZIOM?","ARE YOU SURE YOU WANT TO RESTART THE LEVEL?","VUOI DAVVERO RIAVVIARE IL LIVELLO?","本当にレベルをやり直しますか？","MÖCHTEST DU DAS LEVEL WIRKLICH NEUSTARTEN?"}},
    {"yes", {"TAK","YES","SÌ","はい","JA"}},
    {"no", {"NIE","NO","NO","いいえ","NEIN"}},
    {"game_over", {"GAME OVER","GAME OVER","GAME OVER","ゲームオーバー","GAME OVER"}},
    {"current_story", {"AKTUALNA BAJKA","CURRENT CARTOON","CARTONE ATTUALE","現在のカートゥーン","AKTUELLER CARTOON"}},
    {"restart_story", {"ZACZNIJ BAJKĘ OD NOWA","RESTART CARTOON","RICOMINCIA IL CARTONE","カートゥーンを最初から","CARTOON NEU STARTEN"}},
    {"main_menu", {"MENU GŁÓWNE","MAIN MENU","MENU PRINCIPALE","メインメニュー","HAUPTMENÜ"}},
    {"restart_stats", {"RESTART: %d ŻYCIA   10 KULEK","RESTART: %d LIVES   10 MARBLES","RIAVVIO: %d VITE   10 BIGLIE","リスタート: ライフ %d   ビー玉 10","NEUSTART: %d LEBEN   10 MURMELN"}},
    {"arrows_enter", {"STRZAŁKI / ENTER","ARROWS / ENTER","FRECCE / INVIO","矢印 / ENTER","PFEILE / ENTER"}},
    {"confirm_story", {"CZY NA PEWNO CHCESZ ZACZĄĆ GRĘ OD NOWA?","ARE YOU SURE YOU WANT TO RESTART?","VUOI DAVVERO RICOMINCIARE?","本当に最初からやり直しますか？","MÖCHTEST DU WIRKLICH NEU STARTEN?"}},
    {"profile_reset", {"ZAPIS TEGO PROFILU ZOSTANIE ZRESETOWANY","THIS PROFILE SAVE WILL BE RESET","IL SALVATAGGIO DEL PROFILO SARÀ AZZERATO","このプロフィールのセーブはリセットされます","DIESER PROFIL-SPIELSTAND WIRD ZURÜCKGESETZT"}},
    {"cannot_checkpoint", {"NIE MOŻNA WCZYTAĆ CHECKPOINTU","CANNOT LOAD CHECKPOINT","IMPOSSIBILE CARICARE IL CHECKPOINT","チェックポイントを読み込めません","CHECKPOINT KANN NICHT GELADEN WERDEN"}},
    {"cannot_menu", {"NIE MOŻNA WRÓCIĆ DO MENU","CANNOT RETURN TO MENU","IMPOSSIBILE TORNARE AL MENU","メニューに戻れません","RÜCKKEHR ZUM MENÜ NICHT MÖGLICH"}},
    {"ach_title", {"OSIĄGNIĘCIA RETROACHIEVEMENTS","RETROACHIEVEMENTS","OBIETTIVI RETROACHIEVEMENTS","RETROACHIEVEMENTS 実績","RETROACHIEVEMENTS-ERFOLGE"}},
    {"ra_login", {"ZALOGUJ DO RETROACHIEVEMENTS","SIGN IN TO RETROACHIEVEMENTS","ACCEDI A RETROACHIEVEMENTS","RETROACHIEVEMENTSにログイン","BEI RETROACHIEVEMENTS ANMELDEN"}},
    {"ra_logout", {"WYLOGUJ: %s","SIGN OUT: %s","ESCI: %s","ログアウト: %s","ABMELDEN: %s"}},
    {"ra_not_logged", {"NIEZALOGOWANY — ODBLOKOWANIA NIE SĄ SYNCHRONIZOWANE","NOT SIGNED IN — UNLOCKS ARE NOT SYNCED","NON CONNESSO — GLI OBIETTIVI NON SONO SINCRONIZZATI","未ログイン — 実績は同期されません","NICHT ANGEMELDET — ERFOLGE WERDEN NICHT SYNCHRONISIERT"}},
    {"ra_softcore", {"TRYB SOFTCORE — STORY UŻYWA CHECKPOINTÓW/SAVESTATE","SOFTCORE MODE — STORY USES CHECKPOINT SAVESTATES","MODALITÀ SOFTCORE — LA STORIA USA SAVESTATE","ソフトコア — ストーリーはセーブステートを使用","SOFTCORE — STORY VERWENDET SAVESTATES"}},
    {"ra_wait", {"OCZEKIWANIE NA LOGOWANIE / IDENTYFIKACJĘ GRY","WAITING FOR SIGN-IN / GAME IDENTIFICATION","IN ATTESA DI ACCESSO / IDENTIFICAZIONE DEL GIOCO","ログイン / ゲーム識別待ち","WARTE AUF ANMELDUNG / SPIELERKENNUNG"}},
    {"ra_logged", {"ZALOGOWANO: %s","SIGNED IN: %s","CONNESSO: %s","ログイン中: %s","ANGEMELDET: %s"}},
    {"ra_points", {"PKT","PTS","PT","ポイント","PKT"}},
    {"ra_unlocked", {"ODBLOKOWANE","UNLOCKED","SBLOCCATO","解除済み","FREIGESCHALTET"}},
    {"ra_locked", {"ZABLOKOWANE","LOCKED","BLOCCATO","未解除","GESPERRT"}},
    {"crt_off", {"WYŁ.","OFF","DISATT.","オフ","AUS"}},
    {"crt_classic", {"KLASYCZNY","CLASSIC","CLASSICO","クラシック","KLASSISCH"}},
    {"crt_tv", {"TELEWIZOR","TV","TV","テレビ","FERNSEHER"}},
    {"crt_arcade", {"ARCADE","ARCADE","ARCADE","アーケード","ARCADE"}},
    {"crt_strong", {"MOCNY","STRONG","FORTE","強","STARK"}},
};
static const char* tr(const char* key) {
    if(!key) return "";
    const int lang=std::clamp(g_cfg.language,0,4);
    for(const auto& r:kMickeyTr) if(std::strcmp(r.key,key)==0) return r.v[lang];
    return key;
}
static const char* language_name(int n) {
    static const char* k[5]={"Polski","English","Italiano","日本語","Deutsch"};
    return k[std::clamp(n,0,4)];
}
struct MickeyAchTitleTr { const char* en; const char* v[5]; };
static const MickeyAchTitleTr kMickeyAchTitleTr[] = {
    {"Steamboat Willie", {"Parowiec Willie","Steamboat Willie","Steamboat Willie","蒸気船ウィリー","Steamboat Willie"}},
    {"Will It Bark or Crow, or Cackle?", {"Czy to szczeka, pieje czy gdacze?","Will It Bark or Crow, or Cackle?","Abbaia, canta o chioccia?","吠える？鳴く？それともコケコッコー？","Bellt, kräht oder gackert es?"}},
    {"A Moose!", {"Łoś!","A Moose!","Un alce!","ヘラジカだ！","Ein Elch!"}},
    {"I Ain't a Scared of No Ghosts", {"Nie boję się żadnych duchów","I Ain't a Scared of No Ghosts","Non ho paura dei fantasmi","幽霊なんて怖くない","Ich habe keine Angst vor Geistern"}},
    {"Fee-Fi-Fo-Fum!", {"Fee-Fi-Fo-Fum!","Fee-Fi-Fo-Fum!","Fee-Fi-Fo-Fum!","フィー・ファイ・フォー・ファム！","Fee-Fi-Fo-Fum!"}},
    {"The Vilest Villain That You'd Ever Want to Meet", {"Najpodlejszy złoczyńca, jakiego możesz spotkać","The Vilest Villain That You'd Ever Want to Meet","Il cattivo più vile che potresti incontrare","史上最悪の悪役","Der übelste Schurke, dem du begegnen möchtest"}},
    {"Storm", {"Burza","Storm","Tempesta","嵐","Sturm"}},
    {"Oh Boy!", {"O rany!","Oh Boy!","Oh, ragazzi!","オー・ボーイ！","Oh Junge!"}},
    {"When the Cat's Away, the Mouse Will Play", {"Gdy kota nie ma, myszy harcują","When the Cat's Away, the Mouse Will Play","Quando il gatto non c’è, i topi ballano","猫がいない間にネズミは遊ぶ","Ist die Katze aus dem Haus, tanzt die Maus"}},
    {"Way-Out", {"Wyjście awaryjne","Way-Out","Via d’uscita","脱出口","Ausweg"}},
    {"Steamboat Willie's Secret", {"Sekret Parowca Willie","Steamboat Willie's Secret","Il segreto di Steamboat Willie","蒸気船ウィリーの秘密","Steamboat Willies Geheimnis"}},
    {"Mad Doctor Mickey", {"Miki Szalony Doktor","Mad Doctor Mickey","Mickey dottore pazzo","マッドドクター・ミッキー","Micky, der verrückte Doktor"}},
    {"Secret in the Basement", {"Sekret w piwnicy","Secret in the Basement","Segreto nel seminterrato","地下室の秘密","Geheimnis im Keller"}},
    {"Secret in the Dungeon", {"Sekret w lochu","Secret in the Dungeon","Segreto nel dungeon","ダンジョンの秘密","Geheimnis im Verlies"}},
    {"Marble Collector Mickey", {"Miki — kolekcjoner kulek","Marble Collector Mickey","Mickey collezionista di biglie","ビー玉コレクター・ミッキー","Micky, der Murmelsammler"}},
    {"Watch Your Head", {"Uważaj na głowę","Watch Your Head","Attento alla testa","頭上注意","Kopf einziehen"}},
    {"Hi-Jump", {"Wysoki skok","Hi-Jump","Salto alto","ハイジャンプ","Hochsprung"}},
    {"That's Six of One, Half a Dozen of the Other", {"Sześć jednego, pół tuzina drugiego","That's Six of One, Half a Dozen of the Other","Sei dell’uno, mezza dozzina dell’altro","どっちも同じ","Wie gewonnen, so zerronnen"}},
    {"Seesaw Madness", {"Huśtawkowe szaleństwo","Seesaw Madness","Follia sull’altalena","シーソー大騒ぎ","Wippen-Wahnsinn"}},
    {"Mickey Hat Collector", {"Kolekcjoner czapek Mikiego","Mickey Hat Collector","Collezionista di cappelli di Mickey","ミッキー帽子コレクター","Mickys Hutsammler"}},
    {"You'll See Me Again as a Conqueror!", {"Jeszcze mnie zobaczysz jako zdobywcę!","You'll See Me Again as a Conqueror!","Mi rivedrai come conquistatore!","征服者としてまた会おう！","Du wirst mich als Eroberer wiedersehen!"}},
    {"Dragonfly Rider", {"Jeździec ważki","Dragonfly Rider","Cavaliere della libellula","トンボライダー","Libellenreiter"}},
    {"Run Mickey, Run!", {"Biegnij, Miki, biegnij!","Run Mickey, Run!","Corri Mickey, corri!","走れ、ミッキー！","Lauf, Micky, lauf!"}},
    {"Deathless Mickey", {"Miki bez straty życia","Deathless Mickey","Mickey senza perdere vite","ノーミス・ミッキー","Micky ohne Lebensverlust"}},
    {"Rage Against the Machine", {"Bunt przeciw maszynie","Rage Against the Machine","Ribellione contro la macchina","機械への反逆","Wut gegen die Maschine"}},
    {"DOWN STAIRS Stars", {"Gwiazdy DOWN STAIRS","DOWN STAIRS Stars","Stelle di DOWN STAIRS","DOWN STAIRS のスター","DOWN-STAIRS-Sterne"}},
    {"THE MAD DOCTOR No Hit", {"THE MAD DOCTOR — bez obrażeń","THE MAD DOCTOR No Hit","THE MAD DOCTOR — senza danni","THE MAD DOCTOR ノーダメージ","THE MAD DOCTOR — ohne Treffer"}},
    {"Just Wait! No One and I Do Mean No One Messes W/ The Mighty Pete", {"Poczekaj! Nikt — absolutnie nikt — nie zadziera z potężnym Pete’em","Just Wait! No One and I Do Mean No One Messes W/ The Mighty Pete","Aspetta! Nessuno, e dico nessuno, sfida il potente Pete","待て！最強のピートに手を出すな！","Warte nur! Niemand legt sich mit dem mächtigen Pete an"}},
    {"UP THE TOWER Speedrun", {"UP THE TOWER — próba czasowa","UP THE TOWER Speedrun","UP THE TOWER — prova a tempo","UP THE TOWER タイムアタック","UP THE TOWER — Zeitrennen"}},
    {"DOWN STAIRS Speedrun", {"DOWN STAIRS — próba czasowa","DOWN STAIRS Speedrun","DOWN STAIRS — prova a tempo","DOWN STAIRS タイムアタック","DOWN STAIRS — Zeitrennen"}},
    {"BONUS LEVEL Speedrun", {"BONUS LEVEL — próba czasowa","BONUS LEVEL Speedrun","BONUS LEVEL — prova a tempo","BONUS LEVEL タイムアタック","BONUS LEVEL — Zeitrennen"}},
    {"STEAMBOAT WILLIE No Hit", {"STEAMBOAT WILLIE — bez obrażeń","STEAMBOAT WILLIE No Hit","STEAMBOAT WILLIE — senza danni","STEAMBOAT WILLIE ノーダメージ","STEAMBOAT WILLIE — ohne Treffer"}},
    {"MOOSE HUNTERS No Hit", {"MOOSE HUNTERS — bez obrażeń","MOOSE HUNTERS No Hit","MOOSE HUNTERS — senza danni","MOOSE HUNTERS ノーダメージ","MOOSE HUNTERS — ohne Treffer"}},
    {"MICKEY AND THE BEANSTALK No Hit", {"MICKEY AND THE BEANSTALK — bez obrażeń","MICKEY AND THE BEANSTALK No Hit","MICKEY AND THE BEANSTALK — senza danni","MICKEY AND THE BEANSTALK ノーダメージ","MICKEY AND THE BEANSTALK — ohne Treffer"}},
    {"LONESOME GHOSTS No Hit", {"LONESOME GHOSTS — bez obrażeń","LONESOME GHOSTS No Hit","LONESOME GHOSTS — senza danni","LONESOME GHOSTS ノーダメージ","LONESOME GHOSTS — ohne Treffer"}},
    {"THE PRINCE AND THE PAUPER No Hit", {"THE PRINCE AND THE PAUPER — bez obrażeń","THE PRINCE AND THE PAUPER No Hit","THE PRINCE AND THE PAUPER — senza danni","THE PRINCE AND THE PAUPER ノーダメージ","THE PRINCE AND THE PAUPER — ohne Treffer"}},
};
static std::string replace_all(std::string s,const char* a,const char* b) {
    if(!a||!*a) return s; size_t p=0,n=std::strlen(a);
    while((p=s.find(a,p))!=std::string::npos){s.replace(p,n,b);p+=std::strlen(b);}
    return s;
}
static std::string localize_ach_title(const char* en) {
    if(!en) return {}; const int lang=std::clamp(g_cfg.language,0,4);
    if(lang==1) return en;
    for(const auto& r:kMickeyAchTitleTr) if(std::strcmp(r.en,en)==0) return r.v[lang];
    std::string s=en;
    if(s.size()>8 && s.rfind(" Speedrun")==s.size()-9) {
        s.resize(s.size()-9);
        if(lang==0) return s+" — próba czasowa";
        if(lang==2) return s+" — prova a tempo";
        if(lang==3) return s+" タイムアタック";
        if(lang==4) return s+" — Zeitrennen";
    }
    if(s.size()>7 && s.rfind(" No Hit")==s.size()-7) {
        s.resize(s.size()-7);
        if(lang==0) return s+" — bez obrażeń";
        if(lang==2) return s+" — senza danni";
        if(lang==3) return s+" ノーダメージ";
        if(lang==4) return s+" — ohne Treffer";
    }
    return en;
}
struct MickeyAchDescTr { const char* en; const char* v[5]; };
static const MickeyAchDescTr kMickeyAchDescTr[] = {
    {"Beat the \"first\" cartoon STEAMBOAT WILLIE on any difficulty", {"Ukończ „pierwszą” kreskówkę STEAMBOAT WILLIE na dowolnym poziomie trudności","Beat the \"first\" cartoon STEAMBOAT WILLIE on any difficulty","Completa il \"primo\" cartone STEAMBOAT WILLIE a qualsiasi difficoltà","「最初」のカートゥーン STEAMBOAT WILLIE を任意の難易度でクリアする","Beende den „ersten“ Cartoon STEAMBOAT WILLIE auf beliebigem Schwierigkeitsgrad"}},
    {"Beat the second cartoon THE MAD DOCTOR on any difficulty", {"Ukończ drugą kreskówkę THE MAD DOCTOR na dowolnym poziomie trudności","Beat the second cartoon THE MAD DOCTOR on any difficulty","Completa il secondo cartone THE MAD DOCTOR a qualsiasi difficoltà","2番目のカートゥーン THE MAD DOCTOR を任意の難易度でクリアする","Beende den zweiten Cartoon THE MAD DOCTOR auf beliebigem Schwierigkeitsgrad"}},
    {"Beat the third cartoon MOOSE HUNTERS on any difficulty", {"Ukończ trzecią kreskówkę MOOSE HUNTERS na dowolnym poziomie trudności","Beat the third cartoon MOOSE HUNTERS on any difficulty","Completa il terzo cartone MOOSE HUNTERS a qualsiasi difficoltà","3番目のカートゥーン MOOSE HUNTERS を任意の難易度でクリアする","Beende den dritten Cartoon MOOSE HUNTERS auf beliebigem Schwierigkeitsgrad"}},
    {"Beat the fourth cartoon LONESOME GHOSTS on any difficulty", {"Ukończ czwartą kreskówkę LONESOME GHOSTS na dowolnym poziomie trudności","Beat the fourth cartoon LONESOME GHOSTS on any difficulty","Completa il quarto cartone LONESOME GHOSTS a qualsiasi difficoltà","4番目のカートゥーン LONESOME GHOSTS を任意の難易度でクリアする","Beende den vierten Cartoon LONESOME GHOSTS auf beliebigem Schwierigkeitsgrad"}},
    {"Beat the fifth cartoon MICKEY AND THE BEANSTALK on any difficulty", {"Ukończ piątą kreskówkę MICKEY AND THE BEANSTALK na dowolnym poziomie trudności","Beat the fifth cartoon MICKEY AND THE BEANSTALK on any difficulty","Completa il quinto cartone MICKEY AND THE BEANSTALK a qualsiasi difficoltà","5番目のカートゥーン MICKEY AND THE BEANSTALK を任意の難易度でクリアする","Beende den fünften Cartoon MICKEY AND THE BEANSTALK auf beliebigem Schwierigkeitsgrad"}},
    {"Mess with the mighty Pete and beat the final cartoon THE PRINCE AND THE PAUPER on MEDIUM or HARD difficulty", {"Zadrzyj z potężnym Pete’em i ukończ finałową kreskówkę THE PRINCE AND THE PAUPER na poziomie MEDIUM lub HARD","Mess with the mighty Pete and beat the final cartoon THE PRINCE AND THE PAUPER on MEDIUM or HARD difficulty","Sfida il potente Pete e completa il cartone finale THE PRINCE AND THE PAUPER a difficoltà MEDIUM o HARD","強大なピートに挑み、最終カートゥーン THE PRINCE AND THE PAUPER を MEDIUM または HARD でクリアする","Lege dich mit dem mächtigen Pete an und beende den letzten Cartoon THE PRINCE AND THE PAUPER auf MEDIUM oder HARD"}},
    {"Find and beat the BONUS LEVEL on any difficulty", {"Znajdź i ukończ BONUS LEVEL na dowolnym poziomie trudności","Find and beat the BONUS LEVEL on any difficulty","Trova e completa BONUS LEVEL a qualsiasi difficoltà","BONUS LEVEL を見つけ、任意の難易度でクリアする","Finde und beende BONUS LEVEL auf beliebigem Schwierigkeitsgrad"}},
    {"Beat the game on HARD (3 Lives settings)", {"Ukończ grę na HARD (ustawienie 3 żyć)","Beat the game on HARD (3 Lives settings)","Completa il gioco a HARD (impostazione 3 vite)","HARD（ライフ3設定）でゲームをクリアする","Beende das Spiel auf HARD (Einstellung: 3 Leben)"}},
    {"Find the hidden room in STEAMBOAT WILLIE", {"Znajdź ukryty pokój w STEAMBOAT WILLIE","Find the hidden room in STEAMBOAT WILLIE","Trova la stanza segreta in STEAMBOAT WILLIE","STEAMBOAT WILLIE の隠し部屋を見つける","Finde den geheimen Raum in STEAMBOAT WILLIE"}},
    {"If you feel like a fish out of water just go back in the water", {"Jeśli czujesz się jak ryba bez wody, po prostu wróć do wody","If you feel like a fish out of water just go back in the water","Se ti senti come un pesce fuor d’acqua, torna semplicemente in acqua","水を離れた魚のように感じたら、水に戻ろう","Wenn du dich wie ein Fisch auf dem Trockenen fühlst, geh einfach zurück ins Wasser"}},
    {"Don't rescue Steamboat Willie", {"Nie ratuj Steamboat Willie","Don't rescue Steamboat Willie","Non salvare Steamboat Willie","Steamboat Willie を助けない","Rette Steamboat Willie nicht"}},
    {"Scare away Mad Doctor Mickey", {"Przestrasz Mad Doctor Mickey i przepędź go","Scare away Mad Doctor Mickey","Spaventa e fai scappare Mad Doctor Mickey","Mad Doctor Mickey を怖がらせて追い払う","Erschrecke Mad Doctor Mickey und vertreibe ihn"}},
    {"Find the eight hidden Stars in BASEMENT", {"Znajdź osiem ukrytych gwiazdek w BASEMENT","Find the eight hidden Stars in BASEMENT","Trova le otto stelle nascoste in BASEMENT","BASEMENT で隠された8個のスターを見つける","Finde die acht versteckten Sterne in BASEMENT"}},
    {"Find the \"hidden\" Star in THE DUNGEON", {"Znajdź „ukrytą” gwiazdkę w THE DUNGEON","Find the \"hidden\" Star in THE DUNGEON","Trova la stella \"nascosta\" in THE DUNGEON","THE DUNGEON の「隠し」スターを見つける","Finde den „versteckten“ Stern in THE DUNGEON"}},
    {"Carry 99 Marbles", {"Miej przy sobie 99 kulek","Carry 99 Marbles","Porta con te 99 biglie","ビー玉を99個持つ","Trage 99 Murmeln bei dir"}},
    {"Collect the first Live in STEAMBOAT WILLIE", {"Zbierz pierwsze dodatkowe życie w STEAMBOAT WILLIE","Collect the first Live in STEAMBOAT WILLIE","Raccogli la prima vita extra in STEAMBOAT WILLIE","STEAMBOAT WILLIE で最初のエクストラライフを取る","Sammle das erste Extraleben in STEAMBOAT WILLIE"}},
    {"Collect the well hidden Live in THE MAD DOCTOR", {"Zbierz dobrze ukryte dodatkowe życie w THE MAD DOCTOR","Collect the well hidden Live in THE MAD DOCTOR","Raccogli la vita extra ben nascosta in THE MAD DOCTOR","THE MAD DOCTOR でよく隠されたエクストラライフを取る","Sammle das gut versteckte Extraleben in THE MAD DOCTOR"}},
    {"Collect the first Live in LONESOME GHOSTS", {"Zbierz pierwsze dodatkowe życie w LONESOME GHOSTS","Collect the first Live in LONESOME GHOSTS","Raccogli la prima vita extra in LONESOME GHOSTS","LONESOME GHOSTS で最初のエクストラライフを取る","Sammle das erste Extraleben in LONESOME GHOSTS"}},
    {"Collect the second Live in LONESOME GHOSTS", {"Zbierz drugie dodatkowe życie w LONESOME GHOSTS","Collect the second Live in LONESOME GHOSTS","Raccogli la seconda vita extra in LONESOME GHOSTS","LONESOME GHOSTS で2つ目のエクストラライフを取る","Sammle das zweite Extraleben in LONESOME GHOSTS"}},
    {"Collect all 8 lives in a single playthrough", {"Zbierz wszystkie 8 żyć w jednym podejściu","Collect all 8 lives in a single playthrough","Raccogli tutte le 8 vite in una singola partita","1回のプレイで8個すべてのライフを集める","Sammle alle 8 Leben in einem Durchgang"}},
    {"Destroy the cogwheels without jumping on a coil spring", {"Zniszcz koła zębate bez skakania na sprężynie","Destroy the cogwheels without jumping on a coil spring","Distruggi gli ingranaggi senza saltare su una molla","コイルばねでジャンプせずに歯車を壊す","Zerstöre die Zahnräder, ohne auf eine Sprungfeder zu springen"}},
    {"Collect all 3 stars in DOWN STAIRS", {"Zbierz wszystkie 3 gwiazdki w DOWN STAIRS","Collect all 3 stars in DOWN STAIRS","Raccogli tutte e 3 le stelle in DOWN STAIRS","DOWN STAIRS で3つのスターをすべて集める","Sammle alle 3 Sterne in DOWN STAIRS"}},
    {"Beat Dr. XXX without throwing marbles", {"Pokonaj Dr. XXX bez rzucania kulkami","Beat Dr. XXX without throwing marbles","Sconfiggi Dr. XXX senza lanciare biglie","ビー玉を投げずに Dr. XXX を倒す","Besiege Dr. XXX, ohne Murmeln zu werfen"}},
    {"Cross the water in NICE GARDEN touching only the last leaf", {"Przejdź przez wodę w NICE GARDEN, dotykając tylko ostatniego liścia","Cross the water in NICE GARDEN touching only the last leaf","Attraversa l’acqua in NICE GARDEN toccando solo l’ultima foglia","NICE GARDEN で最後の葉だけに触れて水を渡る","Überquere das Wasser in NICE GARDEN und berühre nur das letzte Blatt"}},
    {"Collect the four stars in UNDERGROUND and sucessfully flee from the Spiderlady", {"Zbierz cztery gwiazdki w UNDERGROUND i skutecznie ucieknij przed Spiderlady","Collect the four stars in UNDERGROUND and sucessfully flee from the Spiderlady","Raccogli le quattro stelle in UNDERGROUND e fuggi con successo dalla Spiderlady","UNDERGROUND で4つのスターを集め、Spiderlady から逃げ切る","Sammle die vier Sterne in UNDERGROUND und entkomme erfolgreich der Spiderlady"}},
    {"Beat DOWN STAIRS in 50.4s (PAL) or 42s (patched NTSC) or less (NORMAL+, Level Select Cheat allowed)", {"Ukończ DOWN STAIRS w 50,4 s (PAL) lub 42 s (zmodyfikowane NTSC) albo szybciej (NORMAL+, kod wyboru poziomu dozwolony)","Beat DOWN STAIRS in 50.4s (PAL) or 42s (patched NTSC) or less (NORMAL+, Level Select Cheat allowed)","Completa DOWN STAIRS in 50,4 s (PAL) o 42 s (NTSC modificato) o meno (NORMAL+, trucco selezione livello consentito)","DOWN STAIRS を50.4秒（PAL）または42秒（NTSCパッチ版）以内でクリアする（NORMAL+、レベル選択チート可）","Beende DOWN STAIRS in höchstens 50,4 s (PAL) oder 42 s (gepatchtes NTSC) (NORMAL+, Levelauswahl-Cheat erlaubt)"}},
    {"Beat UP THE TOWER in 48s (PAL) or 40s (patched NTSC) or less (NORMAL+, Level Select Cheat allowed)", {"Ukończ UP THE TOWER w 48 s (PAL) lub 40 s (zmodyfikowane NTSC) albo szybciej (NORMAL+, kod wyboru poziomu dozwolony)","Beat UP THE TOWER in 48s (PAL) or 40s (patched NTSC) or less (NORMAL+, Level Select Cheat allowed)","Completa UP THE TOWER in 48 s (PAL) o 40 s (NTSC modificato) o meno (NORMAL+, trucco selezione livello consentito)","UP THE TOWER を48秒（PAL）または40秒（NTSCパッチ版）以内でクリアする（NORMAL+、レベル選択チート可）","Beende UP THE TOWER in höchstens 48 s (PAL) oder 40 s (gepatchtes NTSC) (NORMAL+, Levelauswahl-Cheat erlaubt)"}},
    {"Beat BONUS LEVEL in 20.4s PAL or 17s NTSC patched or less (NORMAL+, Level Select Cheat allowed)", {"Ukończ BONUS LEVEL w 20,4 s PAL lub 17 s w zmodyfikowanym NTSC albo szybciej (NORMAL+, kod wyboru poziomu dozwolony)","Beat BONUS LEVEL in 20.4s PAL or 17s NTSC patched or less (NORMAL+, Level Select Cheat allowed)","Completa BONUS LEVEL in 20,4 s PAL o 17 s NTSC modificato o meno (NORMAL+, trucco selezione livello consentito)","BONUS LEVEL を20.4秒（PAL）または17秒（NTSCパッチ版）以内でクリアする（NORMAL+、レベル選択チート可）","Beende BONUS LEVEL in höchstens 20,4 s PAL oder 17 s im gepatchten NTSC (NORMAL+, Levelauswahl-Cheat erlaubt)"}},
    {"Beat STEAMBOAT WILLIE without taking a hit by defeating the crane (Level Select Cheat allowed)", {"Ukończ STEAMBOAT WILLIE bez otrzymania obrażeń, pokonując żurawia (kod wyboru poziomu dozwolony)","Beat STEAMBOAT WILLIE without taking a hit by defeating the crane (Level Select Cheat allowed)","Completa STEAMBOAT WILLIE senza subire colpi, sconfiggendo la gru (trucco selezione livello consentito)","クレーンを倒し、ノーダメージで STEAMBOAT WILLIE をクリアする（レベル選択チート可）","Beende STEAMBOAT WILLIE ohne Treffer, indem du den Kran besiegst (Levelauswahl-Cheat erlaubt)"}},
    {"Beat THE MAD DOCTOR without taking a hit (Level Select Cheat allowed)", {"Ukończ THE MAD DOCTOR bez otrzymania obrażeń (kod wyboru poziomu dozwolony)","Beat THE MAD DOCTOR without taking a hit (Level Select Cheat allowed)","Completa THE MAD DOCTOR senza subire colpi (trucco selezione livello consentito)","THE MAD DOCTOR をノーダメージでクリアする（レベル選択チート可）","Beende THE MAD DOCTOR ohne Treffer (Levelauswahl-Cheat erlaubt)"}},
    {"Beat MOOSE HUNTERS without taking a hit or trip over a rock (Level Select Cheat allowed)", {"Ukończ MOOSE HUNTERS bez otrzymania obrażeń i bez potknięcia się o kamień (kod wyboru poziomu dozwolony)","Beat MOOSE HUNTERS without taking a hit or trip over a rock (Level Select Cheat allowed)","Completa MOOSE HUNTERS senza subire colpi né inciampare su una roccia (trucco selezione livello consentito)","ダメージを受けず、岩につまずかずに MOOSE HUNTERS をクリアする（レベル選択チート可）","Beende MOOSE HUNTERS ohne Treffer und ohne über einen Stein zu stolpern (Levelauswahl-Cheat erlaubt)"}},
    {"Beat LONESOME GHOSTS without taking a hit (Level Select Cheat allowed)", {"Ukończ LONESOME GHOSTS bez otrzymania obrażeń (kod wyboru poziomu dozwolony)","Beat LONESOME GHOSTS without taking a hit (Level Select Cheat allowed)","Completa LONESOME GHOSTS senza subire colpi (trucco selezione livello consentito)","LONESOME GHOSTS をノーダメージでクリアする（レベル選択チート可）","Beende LONESOME GHOSTS ohne Treffer (Levelauswahl-Cheat erlaubt)"}},
    {"Beat MICKEY AND THE BEANSTALK without taking a hit or trip over a mushroom (Level Select Cheat allowed)", {"Ukończ MICKEY AND THE BEANSTALK bez otrzymania obrażeń i bez potknięcia się o grzyba (kod wyboru poziomu dozwolony)","Beat MICKEY AND THE BEANSTALK without taking a hit or trip over a mushroom (Level Select Cheat allowed)","Completa MICKEY AND THE BEANSTALK senza subire colpi né inciampare su un fungo (trucco selezione livello consentito)","ダメージを受けず、キノコにつまずかずに MICKEY AND THE BEANSTALK をクリアする（レベル選択チート可）","Beende MICKEY AND THE BEANSTALK ohne Treffer und ohne über einen Pilz zu stolpern (Levelauswahl-Cheat erlaubt)"}},
    {"Find the Prince and the Pauper and Beat THE ESCAPE! Without Taking a Hit (Level Select Cheat Allowed)", {"Znajdź Księcia i Żebraka oraz ukończ THE ESCAPE! bez otrzymania obrażeń (kod wyboru poziomu dozwolony)","Find the Prince and the Pauper and Beat THE ESCAPE! Without Taking a Hit (Level Select Cheat Allowed)","Trova il Principe e il Povero e completa THE ESCAPE! senza subire colpi (trucco selezione livello consentito)","王子と乞食を見つけ、THE ESCAPE! をノーダメージでクリアする（レベル選択チート可）","Finde den Prinzen und den Bettelknaben und beende THE ESCAPE! ohne Treffer (Levelauswahl-Cheat erlaubt)"}},
    {"Beat FIRST BATTLE and END BATTLE without taking a hit (Level Select Cheat allowed)", {"Ukończ FIRST BATTLE i END BATTLE bez otrzymania obrażeń (kod wyboru poziomu dozwolony)","Beat FIRST BATTLE and END BATTLE without taking a hit (Level Select Cheat allowed)","Completa FIRST BATTLE e END BATTLE senza subire colpi (trucco selezione livello consentito)","FIRST BATTLE と END BATTLE をノーダメージでクリアする（レベル選択チート可）","Beende FIRST BATTLE und END BATTLE ohne Treffer (Levelauswahl-Cheat erlaubt)"}},
    {"Beat the WHOLE GAME without losing a Live", {"Ukończ CAŁĄ GRĘ bez utraty ani jednego życia","Beat the WHOLE GAME without losing a Live","Completa TUTTO IL GIOCO senza perdere una vita","ライフを1つも失わずにゲーム全体をクリアする","Beende das GESAMTE SPIEL, ohne ein Leben zu verlieren"}},
};
static std::string localize_ach_desc(const char* en) {
    if(!en) return {};
    const int lang=std::clamp(g_cfg.language,0,4);
    if(lang==1) return en;
    for(const auto& r:kMickeyAchDescTr) if(std::strcmp(r.en,en)==0) return r.v[lang];
    return en; /* new/changed RA entry: show current server English instead of stale translation */
}
