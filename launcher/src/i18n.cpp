#include "i18n.hpp"
#include <algorithm>

namespace bbport {

static const std::vector<LanguageOption> kLanguages = {
  {"en", "English", Language::English},
  {"pt_BR", "Português (Brasil)", Language::Portuguese},
  {"ru", "Русский", Language::Russian},
  {"zh_CN", "简体中文", Language::Chinese},
  {"fr_FR", "Français", Language::French}
};

const std::vector<LanguageOption>& I18n::GetAvailableLanguages() {
  return kLanguages;
}

I18n& I18n::Instance() {
  static I18n instance;
  return instance;
}

I18n::I18n() {
  LoadTranslations();
}

void I18n::SetLanguage(Language lang) {
  current_lang_ = lang;
}

void I18n::SetLanguageByCode(const std::string& code) {
  if (code.find("pt") == 0) {
    current_lang_ = Language::Portuguese;
  } else if (code.find("ru") == 0) {
    current_lang_ = Language::Russian;
  } else if (code.find("zh") == 0) {
    current_lang_ = Language::Chinese;
  } else if (code.find("fr") == 0) {
    current_lang_ = Language::French;
  } else {
    current_lang_ = Language::English;
  }
}

std::string I18n::GetLanguageCode() const {
  for (const auto& opt : kLanguages) {
    if (opt.lang == current_lang_) {
      return opt.code;
    }
  }
  return "en";
}

const std::string& I18n::Translate(const std::string& key) const {
  auto it = strings_.find(key);
  if (it != strings_.end()) {
    auto lang_it = it->second.find(current_lang_);
    if (lang_it != it->second.end() && !lang_it->second.empty()) {
      return lang_it->second;
    }
    // Fallback to English
    auto en_it = it->second.find(Language::English);
    if (en_it != it->second.end()) {
      return en_it->second;
    }
  }
  return key;
}

void I18n::LoadTranslations() {
  auto add = [this](const std::string& key,
                    const std::string& en,
                    const std::string& pt,
                    const std::string& ru,
                    const std::string& zh,
                    const std::string& fr) {
    strings_[key][Language::English] = en;
    strings_[key][Language::Portuguese] = pt;
    strings_[key][Language::Russian] = ru;
    strings_[key][Language::Chinese] = zh;
    strings_[key][Language::French] = fr;
  };

  // Navigation / Tabs
  add("tab_play", "The Hunt", "A Caçada", "Игра", "游戏", "La Chasse");
  add("tab_graphics", "Display & Graphics", "Tela e Gráficos", "Графика", "显示与图形", "Affichage et Graphismes");
  add("tab_effects", "Effects & Engine", "Efeitos e Motor", "Эффекты", "效果与引擎", "Effets et Moteur");
  add("tab_controls", "Controls & Input", "Controles e Entrada", "Управление", "控制与输入", "Contrôles et Entrées");
  add("tab_mods", "Mods & Patches", "Mods e Patches", "Моды и патчи", "模组与补丁", "Mods et Patchs");
  add("tab_trophies", "Trophies", "Troféus", "Трофеи", "奖杯", "Trophées");
  add("tab_online", "Network & Co-op", "Rede e Co-op", "Онлайн", "网络与联机", "Réseau et Co-op");
  add("tab_log", "Log & Console", "Log e Console", "Журнал", "日志与控制台", "Journal et Console");

  // Play section
  add("launch_game", "LAUNCH GAME", "INICIAR JOGO", "ЗАПУСТИТЬ", "启动游戏", "LANCER LE JEU");
  add("stop_game", "STOP RUNNER", "PARAR JOGO", "ОСТАНОВИТЬ", "停止运行", "ARRÊTER");
  add("game_running", "RUNNING", "EM EXECUÇÃO", "ЗАПУЩЕНО", "运行中", "EN COURS");
  add("game_folder", "Game Folder (CUSA03173 / CUSA00900)", "Pasta do Jogo (CUSA03173 / CUSA00900)", "Папка игры (CUSA03173)", "游戏文件夹（CUSA03173）", "Dossier du jeu (CUSA03173)");
  add("user_folder", "Saves & Cache Folder", "Pasta de Saves e Cache", "Папка сохранений", "存档与缓存文件夹", "Dossier des sauvegardes et du cache");
  add("mods_folder", "Mods Directory", "Diretório de Mods", "Папка модов", "模组目录", "Dossier des mods");
  add("browse", "Browse...", "Procurar...", "Обзор...", "浏览...", "Parcourir...");
  add("extract_pkg", "Extract PKG / Dump", "Extrair PKG / Dump", "Распаковать PKG", "解包 PKG / 转储", "Extraire PKG / Dump");
  add("install_update", "Install Update 1.09", "Instalar Update 1.09", "Установить обновление 1.09", "安装 1.09 更新", "Installer la mise à jour 1.09");
  add("status_ready", "Bloodborne 1.09 Ready", "Bloodborne 1.09 Pronto", "Bloodborne 1.09 Готово к запуску", "Bloodborne 1.09 就绪", "Bloodborne 1.09 Prêt");
  add("status_invalid", "Invalid game directory (eboot.bin / dvdroot_ps4 not found)", "Diretório inválido (eboot.bin / dvdroot_ps4 não encontrados)", "Неверная папка игры", "无效的游戏目录", "Dossier de jeu invalide");
  add("status_update_needed", "Game detected, but update 1.09 is required!", "Jogo detectado, mas a atualização 1.09 é necessária!", "Требуется обновление 1.09!", "需要 1.09 更新！", "Mise à jour 1.09 requise !");

  // Graphics & Display
  add("resolution", "Target Resolution", "Resolução Alvo", "Разрешение", "目标分辨率", "Résolution Cible");
  add("fullscreen", "Fullscreen", "Tela Cheia", "Полноэкранный режим", "全屏", "Plein Écran");
  add("borderless", "Borderless Windowed", "Janela Sem Bordas", "Окно без рамок", "无边框窗口", "Fenêtre Sans Bordure");
  add("vsync", "Vertical Sync (VSync)", "Sincronização Vertical (VSync)", "Вертикальная синхронизация", "垂直同步", "Synchronisation Verticale");
  add("fps_limit", "Framerate Limit", "Limite de FPS", "Ограничение FPS", "帧率限制", "Limite de FPS");
  add("upscaler", "Upscaler Backend", "Tecnologia de Upscaling", "Масштабирование", "超分辨率技术", "Technologie d'Upscaling");
  add("preset", "Quality Preset", "Predefinição de Qualidade", "Профиль качества", "质量预设", "Profil de Qualité");
  add("sharpness", "Sharpness", "Nitidez", "Резкость", "锐化程度", "Netteté");
  add("anti_aliasing", "Anti-Aliasing", "Anti-Aliasing", "Сглаживание", "抗锯齿", "Anti-Crénelage");
  add("vbao", "GT-VBAO Ambient Occlusion", "Oclusão de Ambiente GT-VBAO", "Затенение GT-VBAO", "GT-VBAO 环境光遮蔽", "Occlusion Ambiante GT-VBAO");
  add("aspect_ratio", "Aspect Ratio", "Proporção de Tela", "Соотношение сторон", "宽高比", "Format d'Image");

  // Effects & Performance
  add("motion_blur", "Motion Blur", "Desfoque de Movimento", "Размытие в движении", "动态模糊", "Flou de Mouvement");
  add("bloom", "Bloom Lighting", "Iluminação Bloom", "Свечение (Bloom)", "辉光 (Bloom)", "Éclairage Bloom");
  add("chromatic_aberration", "Chromatic Aberration", "Aberração Cromática", "Хроматическая аберрация", "色差", "Aberration Chromatique");
  add("dof", "Depth of Field (DoF)", "Profundidade de Campo", "Глубина резкости", "景深", "Profondeur de Champ");
  add("ssao", "Screen-Space Ambient Occlusion", "Oclusão de Ambiente SSAO", "SSAO", "屏幕空间环境光遮蔽", "Occlusion Ambiante SSAO");
  add("dynamic_shadows", "Dynamic Shadows", "Sombras Dinâmicas", "Динамические тени", "动态阴影", "Ombres Dynamiques");
  add("low_latency", "Low Latency Mode", "Modo de Baixa Latência", "Режим низкой задержки", "低延迟模式", "Mode Faible Latence");
  add("show_fps", "In-Game FPS Counter", "Contador de FPS no Jogo", "Счётчик FPS в игре", "游戏内帧率显示", "Compteur de FPS en Jeu");

  // Controls
  add("mouse_look", "Enable Mouse Look (Camera)", "Câmera com Mouse (Mouse Look)", "Обзор мышью", "鼠标视角控制", "Contrôle Caméra à la Souris");
  add("mouse_sens", "Mouse Sensitivity", "Sensibilidade do Mouse", "Чувствительность мыши", "鼠标灵敏度", "Sensibilité de la Souris");
  add("invert_y", "Invert Y Axis", "Inverter Eixo Y", "Инвертировать ось Y", "反转 Y 轴", "Inverser l'Axe Y");
  add("invert_x", "Invert X Axis", "Inverter Eixo X", "Инвертировать ось X", "反转 X 轴", "Inverser l'Axe X");
  add("button_prompts", "Controller Button Prompts", "Ícones de Botões do Controle", "Значки кнопок", "手柄按键提示", "Icônes des Boutons");

  // Mods & Patches
  add("mods_title", "Installed Mods (Load Order)", "Mods Instalados (Ordem de Carga)", "Установленные моды", "已安装模组（加载顺序）", "Mods Installés (Ordre de Chargement)");
  add("mods_desc", "Mods are loaded from top to bottom. Uncheck to disable without uninstalling.", "Mods são carregados de cima para baixo. Desmarque para desativar sem desinstalar.", "Моды загружаются сверху вниз. Снимите флажок для отключения.", "模组按从上到下的顺序加载。取消勾选可禁用而无需卸载。", "Les mods se chargent de haut en bas. Décochez pour désactiver sans désinstaller.");
  add("move_up", "Move Up", "Mover para Cima", "Выше", "上移", "Monter");
  add("move_down", "Move Down", "Mover para Baixo", "Ниже", "下移", "Descendre");
  add("no_mods_found", "No mods found in the mods directory.", "Nenhum mod encontrado no diretório de mods.", "Модов не найдено.", "在模组目录中未找到模组。", "Aucun mod trouvé dans le dossier des mods.");
  add("patches_title", "shadPS4 XML Patches", "Patches XML do shadPS4", "XML-патчи shadPS4", "shadPS4 XML 补丁", "Patchs XML shadPS4");

  // Trophies
  add("trophies_progress", "Trophy Completion", "Progresso dos Troféus", "Прогресс трофеев", "奖杯完成度", "Progression des Trophées");
  add("trophy_plat", "Platinum", "Platina", "Платина", "白金", "Platine");
  add("trophy_gold", "Gold", "Ouro", "Золото", "金杯", "Or");
  add("trophy_silver", "Silver", "Prata", "Серебро", "银杯", "Argent");
  add("trophy_bronze", "Bronze", "Bronze", "Бронза", "铜杯", "Bronze");
  add("trophy_unlocked_on", "Unlocked on", "Desbloqueado em", "Открыт", "解锁于", "Débloqué le");
  add("trophy_locked", "Locked", "Bloqueado", "Не открыт", "未解锁", "Verrouillé");
  add("trophies_empty", "No trophies unlocked yet. Slay beasts in Yharnam to earn them!", "Nenhum troféu desbloqueado ainda. Cace feras em Yharnam para conquistá-los!", "Трофеи ещё не открыты. Начните охоту в Ярнаме!", "尚未解锁任何奖杯。在亚楠猎杀野兽以获取奖杯！", "Aucun trophée débloqué pour le moment. Chassez des bêtes à Yharnam !");

  // Online & Co-op
  add("online_mode", "Online Multiplayer Mode", "Modo Multijogador Online", "Сетевой режим", "多人在线模式", "Mode Multijoueur en Ligne");
  add("pool_seamless", "Seamless Hunt + BBSC (Co-op & Invasions)", "Seamless Hunt + BBSC (Co-op e Invasões)", "Seamless Hunt + BBSC", "无缝联机 + BBSC (合作与入侵)", "Chasse Sans Soudure + BBSC");
  add("pool_thd", "The Hunter's Dream Pool", "The Hunter's Dream Pool", "The Hunter's Dream", "猎人梦境联机池", "The Hunter's Dream");
  add("server_address", "Multiplayer Server Address", "Endereço do Servidor Online", "Сервер", "服务器地址", "Adresse du Serveur");
  add("discord_token", "Discord Sign-in Token (Optional)", "Token de Autenticação Discord (Opcional)", "Токен Discord (необязательно)", "Discord 登录令牌（可选）", "Jeton Discord (Facultatif)");
  add("local_bbsc", "Run Local BBSC Co-op Relay Server", "Executar Servidor Relay BBSC Local", "Локальный BBSC-сервер", "运行本地 BBSC 联机中继服务器", "Exécuter le Serveur Relais BBSC Local");

  // Log & Console
  add("clear_log", "Clear", "Limpar", "Очистить", "清除", "Effacer");
  add("copy_log", "Copy to Clipboard", "Copiar para Área de Transferência", "Копировать", "复制到剪贴板", "Copier");
  add("save_log", "Export Log...", "Salvar em Arquivo...", "Сохранить...", "导出日志...", "Exporter le Journal...");
  add("ui_language", "Launcher Language", "Idioma do Launcher", "Язык лаунчера", "启动器语言", "Langue du Lanceur");
}

} // namespace bbport
