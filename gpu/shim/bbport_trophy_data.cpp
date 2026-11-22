// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_trophy_data.h"

#include <algorithm>
#include <cstdlib>

namespace BbTrophy {
namespace {

std::uint64_t Be(std::span<const std::uint8_t> d, std::size_t at, int bytes) {
    std::uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) {
        v = v << 8 | d[at + i];
    }
    return v;
}

// An attribute's value in one start tag ("" when absent).
std::string_view Attribute(std::string_view tag, std::string_view name) {
    const std::string key = " " + std::string(name) + "=\"";
    const auto at = tag.find(key);
    if (at == std::string_view::npos) {
        return {};
    }
    const auto from = at + key.size();
    const auto to = tag.find('"', from);
    return to == std::string_view::npos ? std::string_view{} : tag.substr(from, to - from);
}

std::string Decode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '&') {
            out += text[i];
            continue;
        }
        const auto end = text.find(';', i);
        if (end == std::string_view::npos) {
            out += text[i];
            continue;
        }
        const std::string_view entity = text.substr(i + 1, end - i - 1);
        if (entity == "amp") out += '&';
        else if (entity == "lt") out += '<';
        else if (entity == "gt") out += '>';
        else if (entity == "quot") out += '"';
        else if (entity == "apos") out += '\'';
        else {
            out.append(text.substr(i, end - i + 1)); // numeric and others as they are
        }
        i = end;
    }
    return out;
}

// The text of <element>...</element> inside `body` ("" when absent).
std::string Element(std::string_view body, std::string_view element) {
    const std::string open = "<" + std::string(element) + ">", close = "</" + std::string(element) + ">";
    const auto from = body.find(open);
    if (from == std::string_view::npos) {
        return {};
    }
    const auto to = body.find(close, from + open.size());
    return to == std::string_view::npos ? std::string{}
                                        : Decode(body.substr(from + open.size(), to - from - open.size()));
}

int Number(std::string_view text, int fallback) {
    if (text.empty()) {
        return fallback;
    }
    const std::string s(text);
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    return end == s.c_str() ? fallback : int(v);
}

} // namespace

std::vector<TrpEntry> ReadTrpEntries(std::span<const std::uint8_t> trp) {
    // Header: magic DCA24D00, version, file size (8), entry count, entry size; entries from 0x60.
    if (trp.size() < 0x60 || Be(trp, 0, 4) != 0xDCA24D00u) {
        return {};
    }
    const std::uint64_t count = Be(trp, 16, 4), entry_size = Be(trp, 20, 4);
    if (entry_size < 48 || 0x60 + count * entry_size > trp.size()) {
        return {};
    }
    std::vector<TrpEntry> entries;
    for (std::uint64_t i = 0; i < count; ++i) {
        const std::size_t at = 0x60 + i * entry_size;
        TrpEntry e;
        for (std::size_t c = 0; c < 32 && trp[at + c]; ++c) {
            e.name += char(trp[at + c]);
        }
        e.offset = Be(trp, at + 32, 8);
        e.size = Be(trp, at + 40, 8);
        if (e.offset > trp.size() || e.size > trp.size() - e.offset) {
            return {};
        }
        entries.push_back(std::move(e));
    }
    return entries;
}

std::optional<std::string> NpCommId(std::span<const std::uint8_t> npbind) {
    constexpr std::size_t At = 0x84, Length = 12;
    if (npbind.size() < At + Length) {
        return std::nullopt;
    }
    std::string id(reinterpret_cast<const char*>(npbind.data() + At), Length);
    if (id.rfind("NPWR", 0) != 0) {
        return std::nullopt;
    }
    return id;
}

std::string StripPadding(std::span<const std::uint8_t> plain) {
    std::size_t size = plain.size();
    if (size > 0) {
        const std::uint8_t pad = plain[size - 1];
        if (pad >= 1 && pad <= 16 && pad <= size &&
            std::all_of(plain.end() - pad, plain.end(), [pad](std::uint8_t b) { return b == pad; })) {
            size -= pad;
        }
    }
    return std::string(reinterpret_cast<const char*>(plain.data()), size);
}

std::vector<Trophy> ParseTrophies(std::string_view xml) {
    std::vector<Trophy> trophies;
    std::size_t pos = 0;
    while ((pos = xml.find("<trophy ", pos)) != std::string_view::npos) {
        const auto tag_end = xml.find('>', pos);
        if (tag_end == std::string_view::npos) {
            break;
        }
        const std::string_view tag = xml.substr(pos, tag_end - pos);
        Trophy t;
        t.id = Number(Attribute(tag, "id"), -1);
        const auto grade = Attribute(tag, "ttype");
        t.grade = grade.empty() ? '?' : grade[0];
        t.platinum = Number(Attribute(tag, "pid"), -1);
        t.hidden = Attribute(tag, "hidden") == "yes";
        std::size_t next = tag_end + 1;
        if (tag.back() != '/') { // <trophy ...>...</trophy>: the name and detail inside
            const auto close = xml.find("</trophy>", tag_end);
            const std::string_view body =
                xml.substr(tag_end + 1, (close == std::string_view::npos ? xml.size() : close) - tag_end - 1);
            t.name = Element(body, "name");
            t.detail = Element(body, "detail");
            next = close == std::string_view::npos ? xml.size() : close;
        }
        if (t.id >= 0) {
            trophies.push_back(std::move(t));
        }
        pos = next;
    }
    return trophies;
}

std::vector<int> Unlock(const std::vector<Trophy>& trophies, std::set<int>& unlocked, int id) {
    std::vector<int> announce;
    if (!unlocked.insert(id).second) {
        return announce;
    }
    announce.push_back(id);
    const auto it = std::find_if(trophies.begin(), trophies.end(), [id](const Trophy& t) { return t.id == id; });
    if (it == trophies.end() || it->platinum < 0 || unlocked.contains(it->platinum)) {
        return announce;
    }
    const int platinum = it->platinum;
    const bool complete = std::all_of(trophies.begin(), trophies.end(), [&](const Trophy& t) {
        return t.platinum != platinum || unlocked.contains(t.id);
    });
    if (complete) {
        unlocked.insert(platinum);
        announce.push_back(platinum);
    }
    return announce;
}

std::optional<std::vector<std::uint8_t>> ParseKey(std::string_view hex) {
    while (!hex.empty() && (hex.back() == ' ' || hex.back() == '\r' || hex.back() == '\n' || hex.back() == '"')) {
        hex.remove_suffix(1);
    }
    while (!hex.empty() && (hex.front() == ' ' || hex.front() == '"')) {
        hex.remove_prefix(1);
    }
    if (hex.size() != 32) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> key(16);
    for (std::size_t i = 0; i < 16; ++i) {
        int v = 0;
        for (int k = 0; k < 2; ++k) {
            const char c = hex[i * 2 + k];
            const int d = c >= '0' && c <= '9'   ? c - '0'
                          : c >= 'a' && c <= 'f' ? c - 'a' + 10
                          : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                 : -1;
            if (d < 0) {
                return std::nullopt;
            }
            v = v * 16 + d;
        }
        key[i] = std::uint8_t(v);
    }
    return key;
}

std::vector<Trophy> BloodborneTrophies(int language) {
    struct Entry {
        int id;
        char grade;
        int pid;
        bool hidden;
        const char* en_name;
        const char* en_detail;
        const char* pt_name;
        const char* pt_detail;
        const char* ru_name;
        const char* ru_detail;
    };
    static constexpr Entry kCatalog[40] = {
        {0, 'P', -1, false,
         "Bloodborne", "All trophies acquired. Hats off!",
         "Bloodborne", "Todos os troféus conquistados. Tiramos o chapéu!",
         "Bloodborne", "Получены все трофеи. Снимаем шляпу!"},
        {1, 'G', 0, true,
         "Yharnam Sunrise", "You lived through the hunt, and saw another day.",
         "Nascer do Sol de Yharnam", "Você sobreviveu à caçada e viu um novo dia.",
         "Ярнамский восход", "Вы пережили охоту и встретили новый день."},
        {2, 'G', 0, true,
         "Honouring Wishes", "Captivated by the moon presence, you pledge to watch over the hunter's dream.",
         "Honrando Desejos", "Cativado pela presença da lua, você promete vigiar o sonho do caçador.",
         "Исполнение желаний", "Плененный присутствием луны, вы клянетесь оберегать сон охотника."},
        {3, 'G', 0, true,
         "Childhood's Beginning", "You became an infant Great One, lifting humanity into its next childhood.",
         "Início da Infância", "Você se tornou um Eminente infante, elevando a humanidade à sua próxima infância.",
         "Начало детства", "Вы стали младенцем Великим и привели человечество к новому детству."},
        {4, 'G', 0, true,
         "Yharnam, Pthumerian Queen", "Defeat Yharnam, Blood Queen of the Old Labyrinth.",
         "Yharnam, Rainha Pthumeriana", "Derrote Yharnam, Rainha de Sangue do Antigo Labirinto.",
         "Ярнам, Птумерианская королева", "Победите Ярнам, кровавую королеву Старого лабиринта."},
        {5, 'G', 0, false,
         "Hunter's Essence", "Acquire all hunter weapons.",
         "Essência do Caçador", "Adquira todas as armas de caçador.",
         "Сущность охотника", "Соберите все виды оружия охотника."},
        {6, 'G', 0, false,
         "Hunter's Craft", "Acquire all special hunter tools.",
         "Ofício do Caçador", "Adquira todas as ferramentas especiais de caçador.",
         "Ремесло охотника", "Соберите все особые охотничьи инструменты."},
        {7, 'S', 0, false,
         "Weapon Master", "Acquire a weapon of the highest level.",
         "Mestre das Armas", "Adquira uma arma do nível mais alto.",
         "Мастер оружия", "Получите оружие максимального уровня."},
        {8, 'S', 0, false,
         "Blood Gem Master", "Acquire an extremely precious blood gem.",
         "Mestre das Gemas de Sangue", "Adquira uma gema de sangue extremamente preciosa.",
         "Мастер кровавых самоцветов", "Добудьте невероятно ценный кровавый самоцвет."},
        {9, 'S', 0, false,
         "Rune Master", "Acquire an extremely precious Caryll Rune.",
         "Mestre das Runas", "Adquira uma Runa Caryll extremamente preciosa.",
         "Мастер рун", "Добудьте невероятно ценную руну Кэрилла."},
        {10, 'S', 0, true,
         "Cainhurst", "Gain entry to Cainhurst, the lost and ruined castle.",
         "Cainhurst", "Entre em Cainhurst, o castelo perdido e arruinado.",
         "Кейнхёрст", "Проникните в затерянный замок Кейнхёрст."},
        {11, 'S', 0, true,
         "The Choir", "Gain entry to the realm of the Choir, the high stratum of the Healing Church.",
         "O Coro", "Entre no domínio do Coro, o alto escalão da Igreja da Cura.",
         "Хор", "Проникните в обитель Хора — высшей иерархии Церкви Исцеления."},
        {12, 'S', 0, true,
         "The Source of the Dream", "Discover the abandoned old workshop, the source of the hunter's dream.",
         "A Fonte do Sonho", "Descubra a antiga oficina abandonada, a origem do sonho do caçador.",
         "Источник сна", "Найдите заброшенную старую мастерскую — источник сна охотника."},
        {13, 'S', 0, true,
         "Nightmare Lecture Building", "Gain entry into the Byrgenwerth lecture building, that drifts within the realm of nightmare.",
         "Edifício de Palestras do Pesadelo", "Entre no edifício de palestras de Byrgenwerth, à deriva no reino do pesadelo.",
         "Лекционный корпус кошмара", "Проникните в лекционный корпус Бюргенверта в мире кошмара."},
        {14, 'B', 0, true,
         "Father Gascoigne", "Defeat the beast that once was Father Gascoigne.",
         "Padre Gascoigne", "Derrote a fera que um dia foi o Padre Gascoigne.",
         "Отец Гаскойн", "Победите чудовище, которым стал отец Гаскойн."},
        {15, 'B', 0, true,
         "Vicar Amelia", "Defeat the beast that once was Vicar Amelia.",
         "Vigário Amélia", "Derrote a fera que um dia foi a Vigário Amélia.",
         "Викарий Амелия", "Победите чудовище, которым стала викарий Амелия."},
        {16, 'B', 0, true,
         "Shadow of Yharnam", "Defeat the Shadow of Yharnam.",
         "Sombra de Yharnam", "Derrote a Sombra de Yharnam.",
         "Тень Ярнама", "Победите Тень Ярнама."},
        {17, 'B', 0, true,
         "Rom, the Vacuous Spider", "Defeat Great One: Rom, the Vacuous Spider.",
         "Rom, a Aranha Vazia", "Derrote o Eminente: Rom, a Aranha Vazia.",
         "Ром, Праздный Паук", "Победите Великого: Ром, Праздного Паука."},
        {18, 'B', 0, true,
         "The One Reborn", "Defeat The One Reborn.",
         "O Renascido", "Derrote O Renascido.",
         "Возродившийся", "Победите Возродившегося."},
        {19, 'B', 0, true,
         "Micolash, Host of the Nightmare", "Defeat Micolash, Host of the Nightmare.",
         "Micolash, Anfitrião do Pesadelo", "Derrote Micolash, Anfitrião do Pesadelo.",
         "Миколаш, Хозяин Кошмара", "Победите Миколаша, Хозяина Кошмара."},
        {20, 'B', 0, true,
         "Mergo's Wet Nurse", "Defeat Great One: Mergo's Wet Nurse.",
         "Ama de Leite de Mergo", "Derrote o Eminente: Ama de Leite de Mergo.",
         "Кормилица Мерго", "Победите Великого: Кормилицу Мерго."},
        {21, 'B', 0, true,
         "Cleric Beast", "Defeat Cleric Beast.",
         "Fera Clériga", "Derrote a Fera Clériga.",
         "Церковное чудовище", "Победите Церковное чудовище."},
        {22, 'B', 0, true,
         "Blood-starved Beast", "Defeat Blood-starved Beast.",
         "Fera Sedenta de Sangue", "Derrote a Fera Sedenta de Sangue.",
         "Чудовище-кровоглот", "Победите Чудовище-кровоглота."},
        {23, 'B', 0, true,
         "The Witch of Hemwick", "Defeat the Witch of Hemwick.",
         "A Bruxa de Hemwick", "Derrote a Bruxa de Hemwick.",
         "Ведьма Хемвика", "Победите Ведьму Хемвика."},
        {24, 'B', 0, true,
         "Darkbeast Paarl", "Defeat Darkbeast Paarl.",
         "Fera Negra Paarl", "Derrote a Fera Negra Paarl.",
         "Черное чудовище Паарл", "Победите Черное чудовище Паарл."},
        {25, 'B', 0, true,
         "Amygdala", "Defeat Great One: Amygdala.",
         "Amygdala", "Derrote o Eminente: Amygdala.",
         "Амигдала", "Победите Великого: Амигдалу."},
        {26, 'B', 0, true,
         "Martyr Logarius", "Defeat Martyr Logarius.",
         "Mártir Logarius", "Derrote o Mártir Logarius.",
         "Мученик Логариус", "Победите Мученика Логариуса."},
        {27, 'B', 0, true,
         "Celestial Emissary", "Defeat Great One: Celestial Emissary.",
         "Emissário Celestial", "Derrote o Eminente: Emissário Celestial.",
         "Посланник небес", "Победите Великого: Посланника небес."},
        {28, 'B', 0, true,
         "Ebrietas, Daughter of the Cosmos", "Defeat Great One: Ebrietas, Daughter of the Cosmos.",
         "Ebrietas, Filha do Cosmo", "Derrote o Eminente: Ebrietas, Filha do Cosmo.",
         "Ибраитас, Дочь Космоса", "Победите Великого: Ибраитас, Дочь Космоса."},
        {29, 'B', 0, false,
         "Blood Gem Contact", "Acquire a blood gem that imbues hunter weapons with special strength.",
         "Contato com Gema de Sangue", "Adquira uma gema de sangue que concede força especial às armas.",
         "Контакт с самоцветом", "Получите кровавый самоцвет, наделяющий оружие особой силой."},
        {30, 'B', 0, false,
         "Rune Contact", "Acquire a Caryll Rune that endows hunters with special strength.",
         "Contato com Runa", "Adquira uma Runa Caryll que concede força especial aos caçadores.",
         "Контакт с руной", "Получите руну Кэрилла, наделяющую охотника особой силой."},
        {31, 'B', 0, false,
         "Chalice of Pthumeru", "Acquire the Chalice of Pthumeru that seals the catacombs that form a web deep below Yharnam.",
         "Cálice de Pthumeru", "Adquira o Cálice de Pthumeru que sela as catacumbas sob Yharnam.",
         "Чаша Птумеру", "Получите Чашу Птумеру, открывающую катакомбы под Ярнамом."},
        {32, 'B', 0, false,
         "Chalice of Ailing Loran", "Acquire the Chalice of Ailing Loran that seals the tragic land lost to the sands.",
         "Cálice de Loran Enferma", "Adquira o Cálice de Loran Enferma que sela a terra perdida nas areias.",
         "Чаша Больного Лорана", "Получите Чашу Больного Лорана, открывающую погребенные песками земли."},
        {33, 'B', 0, false,
         "Chalice of Isz", "Acquire the Great Chalice of Isz that seals the home of the cosmic kin.",
         "Cálice de Isz", "Adquira o Grande Cálice de Isz que sela o lar dos parentes cósmicos.",
         "Чаша Исза", "Получите Великую чашу Исза, открывающую обитель космических сородичей."},
        // The Old Hunters DLC (34..39)
        {34, 'G', -1, false,
         "Old Hunter's Essence", "Acquire all old hunter weapons.",
         "Essência do Velho Caçador", "Adquira todas as armas dos velhos caçadores.",
         "Сущность старого охотника", "Соберите все виды оружия старых охотников."},
        {35, 'S', -1, true,
         "Orphan of Kos", "Defeat Great One: Orphan of Kos.",
         "Órfão de Kos", "Derrote o Eminente: Órfão de Kos.",
         "Сирота Кос", "Победите Великого: Сироту Кос."},
        {36, 'B', -1, true,
         "Ludwig, the Holy Blade", "Defeat the beast that was once Ludwig, the Holy Blade.",
         "Ludwig, a Lâmina Sagrada", "Derrote a fera que um dia foi Ludwig, a Lâmina Sagrada.",
         "Людвиг, Священный Клинок", "Победите чудовище, которым стал Людвиг, Священный Клинок."},
        {37, 'B', -1, true,
         "Lady Maria of the Astral Clocktower", "Defeat Lady Maria of the Astral Clocktower.",
         "Lady Maria da Torre do Relógio Astral", "Derrote Lady Maria da Torre do Relógio Astral.",
         "Леди Мария из Астральной часовой башни", "Победите леди Марию из Астральной часовой башни."},
        {38, 'B', -1, true,
         "Living Failures", "Defeat the failed attempts to become Great Ones.",
         "Fracassos Vivos", "Derrote as tentativas fracassadas de se tornarem Eminentes.",
         "Живые неудачи", "Победите неудачные попытки создать Великих."},
        {39, 'B', -1, true,
         "Laurence, the First Vicar", "Defeat the beast that was once Laurence, the First Vicar.",
         "Laurence, o Primeiro Vigário", "Derrote a fera que um dia foi Laurence, o Primeiro Vigário.",
         "Лоуренс, Первый Викарий", "Победите чудовище, которым стал Лоуренс, Первый Викарий."},
    };

    const bool is_pt = (language == 7 || language == 17);
    const bool is_ru = (language == 8);
    std::vector<Trophy> out;
    out.reserve(40);
    for (const auto& e : kCatalog) {
        Trophy t;
        t.id = e.id;
        t.grade = e.grade;
        t.platinum = e.pid;
        t.hidden = e.hidden;
        t.name = is_pt ? e.pt_name : (is_ru ? e.ru_name : e.en_name);
        t.detail = is_pt ? e.pt_detail : (is_ru ? e.ru_detail : e.en_detail);
        out.push_back(std::move(t));
    }
    return out;
}

} // namespace BbTrophy
