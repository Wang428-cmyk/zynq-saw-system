import json,sys,tempfile,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from audit_sessions import audit,is_valid

class AuditTests(unittest.TestCase):
    def test_flags(self):
        self.assertTrue(is_valid('1'))
        self.assertFalse(is_valid('0'))
        self.assertFalse(is_valid(''))
    def test_one_session(self):
        with tempfile.TemporaryDirectory() as x:
            root=Path(x);s=root/'sample';s.mkdir()
            (s/'session.json').write_text(json.dumps({'session_id':'sample','state':'completed','target_rounds':2,'completed_rounds':2,'tracks':[{'track_id':1,'metrics':{'total_count':2,'valid_count':1}}]}))
            (s/'summary.csv').write_text('round,track_id,valid\n1,1,1\n2,1,0\n')
            res=audit(root,root/'out')
            self.assertEqual(res['states'],{'completed':1})
            self.assertEqual(res['sessions_with_issues'],0)
            self.assertEqual(res['sessions'][0]['valid_rows'],1)
    def test_discrepancy_reported(self):
        with tempfile.TemporaryDirectory() as x:
            root=Path(x);s=root/'sample';s.mkdir()
            (s/'session.json').write_text(json.dumps({'tracks':[{'track_id':1,'metrics':{'valid_count':0}}]}))
            (s/'summary.csv').write_text('round,track_id,valid\n1,1,1\n')
            res=audit(root,root/'out')
            self.assertEqual(res['sessions_with_issues'],1)

if __name__=='__main__':unittest.main()
