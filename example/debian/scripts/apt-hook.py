#!/usr/bin/env python

import sys, commands, subprocess
from os import stat
from os import path
import string

msg_prfx = 'fsvs-apt-hook_'

def getLine(list):
  result = []
  for i in list:
    if ('Removing' in i) or ('Setting up' in i) or ('Purging' in i) or ('Configuring' in i):
      line = string.replace(i, '\r', '')
      result.append(line)
  return result

def getLastAptAction():
  logfn = '/var/log/apt/term.log'
  try:
    FILE = open(logfn, 'r')
  except:
    print 'could not open file'
  lineList = FILE.readlines()
  length = len(lineList)
  FILE.close()
  result = []
  curline = lineList[-1]
  if 'Log ended:' in curline:
    cond = False
    i = 1
    while cond == False and (length-i)>0:
      i+=1
      curline = lineList[length-i]
      if not 'Log started:' in curline:
        result.insert(1,curline)
      else:
	cond = True
  msg = getLine(result)
  msg.insert(0, msg_prfx + 'last-apt-action:\n')
  return(msg)

def getDpkgFiles():
  cmd = 'dpkg-deb --contents %s' % pkg_file
  print cmd
  try:
    out = commands.getoutput(cmd)
  except:
    print 'exception running %s' % cmd
    exit()
  list = string.split(out, '\n')
  print list[1]

""" gets "fsvs st" state for working copy /
"""
def getFsvsStatus():
  cmd = 'fsvs st /'
  out = commands.getoutput(cmd)
  list = string.split(out, '\n')
  return list

def getConfigChanges():
  list = getFsvsStatus()
  if len(list) > 0:
    print('The following is a list of files that are changed on dpkg-tasks:')
    for i in list:
      print i
    res = raw_input('Do you want to commit these files? (y/N)')
    if res.lower() == 'y':
      return True
  else:
    return False

def ciConfigChanges(msg):
  ci_file = '/tmp/fsvs_cm'
  
  try:
    FILE = open(ci_file, 'w')
  except:
    print 'could not open file %s' % ci_file

  for line in msg:
    FILE.write(line)  
  FILE.close()

  args =['fsvs', 'ci', '/', '-F', ci_file]
  res = subprocess.Popen(args) 

def checkFsvsEnviron():
  """ check fsvs bin availability
  """
  if not path.exists('/usr/bin/fsvs'):
    print msg_prfx + 'error: no instance of fsvs found'
    quit()

  """ check fsvs configuration
  """
  cmd = 'fsvs / urls dump'
  if not len(commands.getoutput(cmd)) > 0:
    print msg_prfx + 'error: no urls defined for /'
    quit()

  """ check fsvs connectivitiy to repo
  """
  cmd = 'fsvs / remote-status'
  if commands.getstatusoutput(cmd) == '1':
    print msg_prfx + 'error: no repo available'
    quit()
   
if __name__ == '__main__':

  checkFsvsEnviron()
  commitmsg = getLastAptAction()

  if getConfigChanges(): 
    ciConfigChanges(commitmsg)
