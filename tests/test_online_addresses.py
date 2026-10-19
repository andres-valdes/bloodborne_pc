"""Online play's addresses (scripts/online.py): The Hunter's Dream, and a self-hosted game server
on its own port (a BBSC host)."""
from paths import ROOT  # noqa: F401 (puts scripts/ on the path)
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import online


class AddressTests(unittest.TestCase):
    def test_the_hunters_dream_stays_on_https_without_a_port(self):
        self.assertEqual(online.community_address(''), 'https://thehuntersdream.com')
        self.assertEqual(online.community_address('thehuntersdream.com/'), 'https://thehuntersdream.com')
        self.assertEqual(online.game_api_url('https://thehuntersdream.com'),
                         'https://thehuntersdream.com:20443/bb-eu/ss.info')

    def test_a_bbsc_game_server_keeps_its_port(self):
        self.assertEqual(online.community_address('http://10.0.0.5:20443'), 'http://10.0.0.5:20443')
        self.assertEqual(online.game_api_url('http://10.0.0.5:20443'),
                         'http://10.0.0.5:20443/bb-eu/ss.info')
        with tempfile.TemporaryDirectory() as user, \
                mock.patch.object(online, 'summon_service', return_value=True):
            path = online.write_overrides(user, 'http://10.0.0.5:20443', 'http://10.0.0.5:31315')
            overrides = json.loads(Path(path).read_text())
        self.assertEqual(overrides[online.GAME_SERVER], 'http://10.0.0.5:20443')

    def test_the_webapi_of_a_bbsc_host_is_kept(self):
        self.assertEqual(online.webapi_setting('http://10.0.0.5:31315', '10.0.0.5:31313',
                                               'http://10.0.0.5:20443'), 'http://10.0.0.5:31315')


if __name__ == '__main__':
    unittest.main()
