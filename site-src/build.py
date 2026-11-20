import re, json, sys, os
from bs4 import BeautifulSoup, NavigableString, Tag
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from jsstrings import JS
HERE = os.path.dirname(os.path.abspath(__file__))
SITE = os.path.join(HERE, '..', 'site')
LANGS = ['en', 'de', 'fr', 'pt', 'ja', 'it', 'ca']
src = open(os.path.join(HERE, 'index.src.html'), encoding='utf8').read()
cut = src.index('</template>') + len('</template>')
head, rest = src[:cut], src[cut:]
head = head.replace('<meta name="description" content="', '<meta name="description" content="', 1)
soup = BeautifulSoup(rest, 'html.parser')
D = {}
cnt = {}
SKIP_TAGS = {'pre', 'script', 'style', 'option', 'select', 'canvas'}
def sec_of(el):
    for p in [el] + list(el.parents):
        if isinstance(p, Tag):
            if p.name == 'section' and p.get('id'): return p['id'].replace('-', '')
            if p.name == 'header': return 'nav'
            if p.name == 'footer': return 'foot'
            if p.get('class') and 'hero' in p.get('class'): return 'hero'
    return 'x'
def inner(el):
    s = ''.join(str(c) for c in el.contents)
    return re.sub(r'\s+', ' ', s).strip()
def keyed(el):
    return el.has_attr('data-i18n') or el.has_attr('data-i18n-html')
def newkey(el):
    s = sec_of(el); cnt[s] = cnt.get(s, 0) + 1
    return '%s.%d' % (s, cnt[s])
def translatable(txt):
    plain = re.sub(r'<[^>]+>', '', txt)
    plain = re.sub(r'&\w+;', '', plain)
    return bool(re.search(r'[a-záéíóúñüçàèòï]', plain))
def walk(el):
    for ch in list(el.children):
        if not isinstance(ch, Tag): continue
        if ch.name in SKIP_TAGS: continue
        if ch.get('data-no-i18n') is not None: continue
        if ch.has_attr('data-i18n') or ch.has_attr('data-i18n-html'):
            D[ch.get('data-i18n') or ch.get('data-i18n-html')] = inner(ch); continue
        # attributes
        attrs = []
        for a in ('aria-label', 'alt', 'title'):
            v = ch.get(a)
            if v and translatable(v):
                k = newkey(ch); D[k] = v; attrs.append('%s:%s' % (a, k))
        if attrs: ch['data-i18n-attr'] = '|'.join(attrs)
        direct = ''.join(str(c) for c in ch.children if isinstance(c, NavigableString)).strip()
        has_tags = any(isinstance(c, Tag) for c in ch.children)
        if direct and not ch.name in ('code', 'kbd'):
            html = inner(ch)
            if translatable(html) and ch.get('class') != ['brand'] and html not in ('Looper 606', 'GitHub'):
                k = newkey(ch); D[k] = html
                ch['data-i18n-html' if has_tags else 'data-i18n'] = k
                continue
        walk(ch)
walk(soup)
sel = soup.find(id='lang'); sel['data-i18n-attr'] = 'aria-label:nav.lang'; D['nav.lang'] = 'Idioma'
# remove SVG-internal title duplicates is not needed. Output.
body = str(soup)
body = body.replace('<script src="assets/app.js"></script>', '<script src="assets/i18n/es.js"></script>\n<script src="assets/app.js"></script>')
open(SITE + '/index.html', 'w', encoding='utf8').write(head + body)
# texts for static attributes handled by JS: meta
D.update(JS)
os.makedirs(SITE + '/assets/i18n', exist_ok=True)
def dump(d, lang):
    return 'window.I18N=window.I18N||{};\nI18N.%s=%s;\n' % (lang, json.dumps(d, ensure_ascii=False, indent=0, sort_keys=False))
open(SITE + '/assets/i18n/es.js', 'w', encoding='utf8').write(dump(D, 'es'))
for l in LANGS:
    tr = json.load(open(os.path.join(HERE, 'translations', l + '.json'), encoding='utf8'))
    miss = [k for k in D if k not in tr]; extra = [k for k in tr if k not in D]
    if miss or extra: print('WARN', l, 'missing', miss[:8], len(miss), 'extra', extra[:8], len(extra))
    open(SITE + '/assets/i18n/%s.js' % l, 'w', encoding='utf8').write(dump({k: v for k, v in tr.items() if k in D}, l))
print(len(D), 'keys;', len(JS), 'from JS')
