git pull origin
git fetch upstream
git checkout master
git merge upstream/master
git commit -m "Merged upstream" . 
git push origin master