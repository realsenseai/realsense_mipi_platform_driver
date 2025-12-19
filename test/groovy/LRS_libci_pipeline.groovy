import org.jenkinsci.plugins.workflow.steps.FlowInterruptedException

pipeline {
    agent { label 'jetson && jetpack && camera' }
    
    options { 
        timestamps()
        timeout(time: 10, unit: 'MINUTES')
    }

    stages {
        stage('pytest') {
            steps {
                script {
					sh 'pytest --tb=no test'
                }
            }
        }
    }
}
