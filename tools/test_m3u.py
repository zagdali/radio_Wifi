text='''#EXTM3U
#EXTINF:-1,Example Radio
http://stream.example.com/stream
#EXTINF:-1,Duplicate Radio
http://stream.example.com/stream
#EXTINF:-1,Invalid stream
not-a-url
# some comment
http://another.example.com/audio
'''

lines = text.splitlines()
stations = []
duplicateCount = 0
invalidCount = 0
pendingName = ''
added = 0

def is_http(url):
    u = url.strip().lower()
    return u.startswith('http://') or u.startswith('https://')

def station_exists(url):
    for s in stations:
        if s['url'] == url:
            return True
    return False

for line in lines:
    line = line.replace('\r', '').strip()
    if not line:
        continue
    if line.startswith('#EXTM3U'):
        continue
    if line.startswith('#EXTINF:'):
        comma = line.find(',')
        pendingName = line[comma+1:].strip() if comma >= 0 else ''
        continue
    if line.startswith('#'):
        continue
    if not is_http(line):
        invalidCount += 1
        pendingName = ''
        continue
    if station_exists(line):
        duplicateCount += 1
        pendingName = ''
        continue
    name = pendingName if pendingName else line.split('/')[-1].split('?')[0]
    stations.append({'name': name, 'url': line})
    added += 1
    pendingName = ''

print('added=', added)
print('duplicates=', duplicateCount)
print('invalid=', invalidCount)
print('stations=')
for s in stations:
    print('-', s['name'], s['url'])
