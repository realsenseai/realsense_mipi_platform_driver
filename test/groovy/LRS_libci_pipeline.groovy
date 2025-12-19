pipeline {
    agent { label 'jetson && jetpack && camera' }
    
    options { 
        timestamps()
        parallelsAlwaysFailFast()
        timeout(time: 10, unit: 'MINUTES')
    }

    stages {
        stage('pytest') {
            steps {
                script {
					sh{
						pytest
					}
                }
            }
        }
    }

    post {
        always {
            script {
                // see https://plugins.jenkins.io/email-ext/
                
                println 'params.GIT_TARGET_BRANCH = ' + params.GIT_TARGET_BRANCH
                println 'params.GIT_BRANCH = ' + params.GIT_BRANCH
                
                def run_on_branch = (params.GIT_TARGET_BRANCH != '') ? params.GIT_TARGET_BRANCH : params.GIT_BRANCH
                name = params.BUILD_NAME + " on " + run_on_branch
                name += " #${env.BUILD_NUMBER}"

                if( params.EMAIL_RECIPIENTS != '' )
                {
                    mail(
                        to: params.EMAIL_RECIPIENTS,
                        subject: "${currentBuild.currentResult}: ${name}",
                        mimeType: "text/html"
                        )
                }

            }
        }
    }
}
