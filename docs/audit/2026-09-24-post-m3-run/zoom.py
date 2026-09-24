exec(open(__file__.replace('zoom.py', 'drive_base.py')).read())
click('Recover'); wait(8, 'initial thumbnails')
log('ZOOM START')
for i in range(12):
    act('zoom-in', pause=0.15)
for i in range(12):
    act('zoom-out', pause=0.15)
log('ZOOM END'); wait(20, 'let the worker drain')
proc.terminate(); proc.wait(30)
