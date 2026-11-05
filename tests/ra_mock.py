"""Loopback-only rc_client service with wholly synthetic achievements and badges."""
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import struct
import threading
import time
from urllib.parse import parse_qs
import zlib


def badge_png():
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    rows = b''.join(b'\0' + bytes([70 + y * 2, 135, 195, 255]) * 64 for y in range(64))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 64, 64, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


class Service:
    def __init__(self):
        self.requests = []
        self.mode = 'ok'
        self.awards = []
        self.badge_requests = 0
        self.badge = badge_png()
        self.unknown = False
        self.set_failure = False
        self.delay = 0
        self.award_mode = 'ok'
        self.url = ''

    def achievement_set(self):
        entries = []
        for number, title, definition, points in [
            (101, 'First signal', '0xH000100=7', 5),
            (102, 'Measured signal', 'M:0xH000100>=10', 10),
            (103, 'Saved signal', '0xH000101=42', 2),
        ]:
            entries.append(dict(ID=number, Title=title, Description='Synthetic memory contract.',
                                Flags=3, Points=points, MemAddr=definition, Author='Fixture',
                                BadgeName=str(number), Created=1, Modified=1,
                                BadgeURL=f'{self.url}/Badge/{number}.png'))
        return dict(Success=True, GameId=1234, Title='Synthetic adventure', ConsoleId=12,
                    ImageIconUrl=f'{self.url}/Images/1.png', RichPresenceGameId=1234,
                    RichPresencePatch='', Sets=[dict(AchievementSetId=1, GameId=1234, Title=None,
                    Type='core', ImageIconUrl=f'{self.url}/Images/1.png', Achievements=entries,
                    Leaderboards=[dict(ID=501, Title='Hidden board', Description='Never submitted',
                    Mem='STA:0xH000100=1::CAN:0xH000100=2::SUB:0xH000100=7::VAL:0xH000100', Format='SCORE')])])

    def response(self, params):
        if self.delay:
            time.sleep(self.delay)
        if self.mode == 'offline':
            return 503, dict(Success=False, Error='Maintenance')
        if self.mode == 'blocked':
            return 403, dict(Success=False, Error='Client blocked', Code='access_denied')
        if self.mode == 'blocked-json':
            return 200, dict(Success=False, Error='Client blocked', Code='access_denied')
        if self.mode == 'expired':
            return 200, dict(Success=False, Error='Token expired', Code='expired_token')
        operation = params.get('r')
        if operation == 'awardachievement' and self.award_mode == 'offline':
            return 503, dict(Success=False, Error='Maintenance')
        if operation == 'login2':
            if params.get('p') != 'test-password' and params.get('t') != 'test-token':
                return 200, dict(Success=False, Error='Invalid credentials', Code='invalid_credentials')
            return 200, dict(Success=True, User='TestUser', Token='test-token', Score=0, SoftcoreScore=2,
                             Messages=0, Permissions=1, AccountType='Registered')
        if operation == 'achievementsets':
            if self.set_failure:
                return 503, dict(Success=False, Error='Maintenance')
            if self.unknown:
                return 200, dict(Success=False, Error='Unknown game', Code='not_found', Status=404)
            return 200, self.achievement_set()
        if operation == 'startsession':
            return 200, dict(Success=True, Unlocks=[], HardcoreUnlocks=[], ServerNow=int(time.time()))
        if operation == 'awardachievement':
            self.awards.append(params)
            return 200, dict(Success=True, AchievementID=int(params['a']), Score=0, SoftcoreScore=5,
                             AchievementsRemaining=2)
        if operation == 'gameid':
            return 200, dict(Success=True, GameID=0 if self.unknown else 1234)
        if operation == 'ping':
            return 200, dict(Success=True)
        return 400, dict(Success=False, Error='Unexpected mock operation')


@contextmanager
def mock_service():
    service = Service()

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_POST(self):
            data = self.rfile.read(int(self.headers.get('Content-Length', 0))).decode()
            params = {k: v[0] for k, v in parse_qs(data).items()}
            service.requests.append((self.path, params, self.headers.get('User-Agent')))
            status, response = service.response(params)
            self.reply(status, json.dumps(response).encode(), 'application/json')

        def do_GET(self):
            service.badge_requests += 1
            self.reply(200, service.badge, 'image/png')

        def reply(self, status, body, mime):
            try:
                self.send_response(status)
                self.send_header('Content-Type', mime)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass

    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    service.url = f'http://127.0.0.1:{server.server_port}'
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield service
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
