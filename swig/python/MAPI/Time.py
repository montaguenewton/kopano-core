import sys
import time

NANOSECS_BETWEEN_EPOCH = 116444736000000000

def _convert(s):
    if sys.hexversion >= 0x03000000 and isinstance(s, bytes):
        return s.decode('ascii')
    else:
        return s

class FileTime(object):
    def __init__(self, filetime):
        self.filetime = filetime
        
    def __getattr__(self, attr):
        if attr == 'unixtime':
            return (self.filetime - NANOSECS_BETWEEN_EPOCH) / 10000000;
        else:
            raise AttributeError
        
    def __setstate__(self, d):
        # XXX pickle with python2, unpickle with python3 (encoding='bytes')
        for k, v in d.items():
            setattr(self, _convert(k), v)

    def __setattr__(self, attr, val):
        if attr == 'unixtime':
            self.filetime = val * 10000000 + NANOSECS_BETWEEN_EPOCH
        else:
            object.__setattr__(self, attr, val)
            
    def __repr__(self):
        try:
            return time.strftime("%Y/%m/%d %H:%M:%S GMT", time.gmtime(self.unixtime))
        except ValueError:
            return '%d' % (self.filetime)
    def __cmp__(self, other):
        return cmp(self.filetime, other.filetime)
        

    def __eq__(self, other):
        return self.__dict__ == other.__dict__

def unixtime(secs):
    t = FileTime(0)
    t.unixtime = secs
    return t
