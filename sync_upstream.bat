git pull origin
git fetch upstream
git checkout master
git merge upstream/master
echo %ERRORLEVEL%
git commit -m "Merged upstream" . 
echo %ERRORLEVEL%
git push origin master
echo %ERRORLEVEL%