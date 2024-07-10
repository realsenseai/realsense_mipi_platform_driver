#!/usr/bin/env python3

'''
This script helps running the tests using pytest.
'''

import sys, os, subprocess, re, getopt, time

start_time = time.time()
running_on_ci = False
if 'WORKSPACE' in os.environ:
    running_on_ci = True

#logs are stored @ ./realsense_mipi_driver_platform/test/logs
logdir = os.path.join( '/'.join(os.path.abspath( __file__ ).split( os.path.sep )[0:-1]), 'logs')
dir_live_tests = os.path.dirname(__file__)

regex = None
handle = None
test_ran = False

def usage():
    ourname = os.path.basename( sys.argv[0] )
    print( 'Syntax: ' + ourname + ' [options] ' )
    print( 'Options:' )
    print( '        -h, --help      Usage help' )
    print( '        -r, --regex     Run all tests whose name matches the following regular expression' )
    print( '                        e.g.: --regex test_fw_version; -r test_fw_version')
        
    sys.exit( 0 )

def command(dev_name, test=None):
    cmd =  ['pytest']
    cmd += ['-s']
    cmd += ['-m', ''.join(dev_name)]
    if test:
        cmd += ['-k', f'{test}'] 
    cmd += [''.join(dir_live_tests)]
    cmd += ['--debug']
    cmd += [f'--junit-xml={logdir}/{dev_name.upper()}_pytest.xml']
    return cmd

def run_test(cmd, test=None, dev_name=None, stdout=None, append =False):
    handle = None
    try:
        if test:
            stdout = stdout + os.sep + str(dev_name.upper()) + '_' + test + '.log'
        else:
            stdout = stdout + os.sep + str(dev_name.upper()) + '_' + 'full.log' 
        if stdout is None:
            sys.stdout.flush()
        elif stdout and stdout != subprocess.PIPE:
            if append:
                handle = open( stdout, "a" )
                handle.write(
                    "\n----------TEST-SEPARATOR----------\n\n" )
                handle.flush()
            else:
                handle = open( stdout, "w" )

        result = subprocess.run( cmd,
                stdout=handle,
                stderr=subprocess.STDOUT,
                universal_newlines=True,
                timeout=200,
                check=True )
        if not result.returncode:
                print("---Test Passed---")
    except Exception as e:
            print("---Test Failed---")
            print( "Error Exception:\n ",e )
                        
    finally:
        if handle:
            handle.close()      

def run_tests_on_d457():     
    global logdir

    try:
        os.makedirs( logdir, exist_ok=True ) 
        device = "D457"  
        
        testname = regex if regex else None

        cmd = command(device.lower(), testname)
        run_test(cmd, testname, device, stdout=logdir, append =False)

    finally:
        if running_on_ci:
            print("Log path- \"Build Artifacts\":/realsense_mipi_driver_platform/test/logs ")
        else:
            print("log path:", logdir)
        run_time = time.time() - start_time
        print( "server took", run_time, "seconds" )

if __name__ == '__main__':
    try:
        opts, args = getopt.getopt( sys.argv[1:], 'hr:', longopts=['help', 'regex=' ] )
    except getopt.GetoptError as err:
        print( err )
        usage()

    for opt, arg in opts:
        if opt in ('-h', '--help'):
            usage()
        elif opt in ('-r', '--regex'):
            regex = arg
    
    run_tests_on_d457()

sys.exit( 0 )
