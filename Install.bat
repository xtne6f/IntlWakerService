cd /d "%~dp0"
sc create IntlWakerService start= auto binPath= "%cd%\IntlWakerService.exe"
sc start IntlWakerService
@pause
